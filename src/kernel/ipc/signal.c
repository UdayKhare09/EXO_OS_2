/*
 * signal.c — per-task signal delivery for EXO_OS
 *
 * Supports both kernel-context dispatch (for kernel tasks) and proper
 * user-space signal delivery (push sigframe onto user stack, redirect
 * execution to handler, restore via rt_sigreturn).
 */
#include "signal.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "mm/kmalloc.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "arch/x86_64/idt.h"  /* cpu_regs_t */
#include "fs/vfs.h"            /* EFAULT, EINVAL */
#include <stddef.h>

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

sig_handler_t *signal_table_alloc(void) {
    sig_handler_t *tbl = kmalloc(NSIGS * sizeof(sig_handler_t));
    if (!tbl) return NULL;
    for (int i = 0; i < NSIGS; i++) tbl[i] = SIG_DFL;
    return tbl;
}

void signal_table_free(sig_handler_t *tbl) {
    kfree(tbl);
}

kernel_sigaction_t *sigaction_table_alloc(void) {
    kernel_sigaction_t *tbl = kmalloc(NSIGS * sizeof(kernel_sigaction_t));
    if (!tbl) return NULL;
    memset(tbl, 0, NSIGS * sizeof(kernel_sigaction_t));
    return tbl;
}

void sigaction_table_free(kernel_sigaction_t *tbl) {
    kfree(tbl);
}

/* ── Handler management ──────────────────────────────────────────────────── */

void signal_set(task_t *t, int sig, sig_handler_t handler) {
    if (!t || sig <= 0 || sig >= NSIGS) return;
    if (sig == SIGKILL) return;   /* uncatchable */
    if (!t->sig_handlers) return;
    t->sig_handlers[sig] = handler;
}

sig_handler_t signal_get(task_t *t, int sig) {
    if (!t || sig <= 0 || sig >= NSIGS || !t->sig_handlers) return SIG_DFL;
    return t->sig_handlers[sig];
}

/* ── Delivery ────────────────────────────────────────────────────────────── */

void signal_send_from_cred(task_t *t, int sig, const cred_t *sender_cred) {
    if (!t || sig <= 0 || sig >= NSIGS) return;

    if (sender_cred) {
        if (!capable(sender_cred, CAP_KILL) &&
            sender_cred->euid != t->cred.uid &&
            sender_cred->uid  != t->cred.uid) {
            return;
        }
    }

    /* Atomically set the pending bit */
    __atomic_or_fetch(&t->sig_pending, (1ULL << sig), __ATOMIC_SEQ_CST);
    /* Wake the task so it dispatches the signal from task context */
    sched_unblock(t);
}

/* Send signal to every non-kthread task whose pgid matches pgid. */
void signal_send_pgrp(uint32_t pgid, int sig) {
    if (pgid == 0 || sig <= 0 || sig >= NSIGS) return;
    for (uint32_t i = 1; i < TASK_TABLE_SIZE; i++) {
        task_t *t = task_get_from_table(i);
        if (!t || t->is_kthread) continue;
        if (t->pgid == pgid) signal_send(t, sig);
    }
}

/* Send signal to every non-kthread task whose sid matches sid. */
void signal_send_session(uint32_t sid, int sig) {
    if (sid == 0 || sig <= 0 || sig >= NSIGS) return;
    for (uint32_t i = 1; i < TASK_TABLE_SIZE; i++) {
        task_t *t = task_get_from_table(i);
        if (!t || t->is_kthread) continue;
        if (t->sid == sid) signal_send(t, sig);
    }
}

/* Default signal action: signals that terminate the process */
static bool sig_default_is_fatal(int sig) {
    switch (sig) {
        case SIGHUP: case SIGINT: case SIGQUIT: case SIGILL:
        case SIGABRT: case SIGFPE: case SIGKILL: case SIGSEGV:
        case SIGPIPE: case SIGALRM: case SIGTERM: case SIGUSR1:
        case SIGUSR2: case SIGBUS: case SIGXCPU: case SIGXFSZ:
            return true;
        default:
            return false;
    }
}

/* Kernel-mode dispatch (for kernel tasks only) */
void signal_dispatch(task_t *t) {
    if (!t) return;

    /* Atomically snapshot and clear all pending bits */
    uint64_t pending = __atomic_exchange_n(&t->sig_pending, 0ULL, __ATOMIC_SEQ_CST);
    if (!pending) return;

    for (int sig = 1; sig < NSIGS; sig++) {
        if (!(pending & (1ULL << sig))) continue;

        /* SIGKILL: mark dead — scheduler will clean up */
        if (sig == SIGKILL) {
            t->state = TASK_DEAD;
            return;
        }

        if (!t->sig_handlers) continue;
        sig_handler_t h = t->sig_handlers[sig];
        if (h == SIG_DFL) {
            if (sig_default_is_fatal(sig)) {
                t->exit_status = sig;
                t->state = TASK_DEAD;
                return;
            }
            continue; /* default ignore (SIGCHLD, etc.) */
        }
        if (h == SIG_IGN) continue;
        h(sig);   /* call handler in current (task) context */
    }
}

/* User-space signal delivery: push a sigframe onto the user stack and
 * redirect execution to the signal handler. Returns true if a signal
 * was actually delivered. */
bool signal_deliver_user(task_t *t, void *regs_ptr) {
    if (!t || !t->is_user || !regs_ptr) return false;

    cpu_regs_t *regs = (cpu_regs_t *)regs_ptr;

    /* Check for pending, unmasked signals */
    uint64_t pending = __atomic_load_n(&t->sig_pending, __ATOMIC_SEQ_CST);
    uint64_t deliverable = pending & ~t->sig_mask;
    if (!deliverable) return false;

    for (int sig = 1; sig < NSIGS; sig++) {
        if (!(deliverable & (1ULL << sig))) continue;

        /* Clear the pending bit */
        __atomic_and_fetch(&t->sig_pending, ~(1ULL << sig), __ATOMIC_SEQ_CST);

        /* SIGKILL: terminate immediately */
        if (sig == SIGKILL) {
            t->exit_status = sig;
            if (t->parent) {
                t->state = TASK_ZOMBIE;
                signal_send(t->parent, SIGCHLD);
                sched_unblock(t->parent);
            } else {
                t->state = TASK_DEAD;
            }
            return true;
        }

        /* Look up sigaction (prefer sigactions table, fall back to sig_handlers) */
        kernel_sigaction_t *sa = NULL;
        if (t->sigactions && t->sigactions[sig].sa_handler != 0) {
            sa = &t->sigactions[sig];
        }

        uint64_t handler_addr = 0;
        uint64_t restorer_addr = 0;
        uint64_t sa_mask = 0;
        uint64_t sa_flags = 0;

        if (sa) {
            handler_addr  = sa->sa_handler;
            restorer_addr = sa->sa_restorer;
            sa_mask       = sa->sa_mask;
            sa_flags      = sa->sa_flags;
        } else if (t->sig_handlers) {
            sig_handler_t h = t->sig_handlers[sig];
            if (h == SIG_DFL) {
                if (sig_default_is_fatal(sig)) {
                    t->exit_status = sig;
                    if (t->parent) {
                        t->state = TASK_ZOMBIE;
                        signal_send(t->parent, SIGCHLD);
                        sched_unblock(t->parent);
                    } else {
                        t->state = TASK_DEAD;
                    }
                    return true;
                }
                continue; /* default ignore */
            }
            if (h == SIG_IGN) continue;
            handler_addr = (uint64_t)h;
        } else {
            /* No handler data at all — apply default */
            if (sig_default_is_fatal(sig)) {
                t->exit_status = sig;
                t->state = TASK_DEAD;
                return true;
            }
            continue;
        }

        if (handler_addr <= 1) {
            /* SIG_DFL or SIG_IGN in sigaction form */
            if (handler_addr == 0 && sig_default_is_fatal(sig)) {
                t->exit_status = sig;
                t->state = TASK_DEAD;
                return true;
            }
            continue;
        }

        /* ── Build sigframe on user stack ─────────────────────────────── */
        /* If SA_ONSTACK and the task has an active alternate signal stack,
         * deliver on the altstack (top = base + size).  Otherwise use the
         * current user RSP.  Matches Linux behaviour for glibc/pthreads. */
        uintptr_t user_sp;
        if ((sa_flags & SA_ONSTACK) && t->altstack_sp &&
            !(t->altstack_flags & SS_DISABLE)) {
            /* Top of alternate stack: base + size, then let alignment below apply */
            user_sp = (uintptr_t)(t->altstack_sp + t->altstack_size);
        } else {
            user_sp = regs->rsp;
        }

        /* Ensure 16-byte alignment, then allocate sigframe */
        user_sp = (user_sp - sizeof(sigframe_t)) & ~0xFULL;
        /* Verify stack is mapped */
        uint64_t *pte = vmm_get_pte(t->cr3, user_sp);
        if (!pte) return false;

        uintptr_t phys = (*pte) & VMM_PTE_ADDR_MASK;
        uintptr_t off  = user_sp & (PAGE_SIZE - 1);

        /* Build the frame - we may cross a page boundary for large frames,
         * but sigframe_t is < 4096 bytes so we check both ends */
        uint64_t *pte_end = vmm_get_pte(t->cr3, user_sp + sizeof(sigframe_t) - 1);
        if (!pte_end) return false;

        sigframe_t *frame = (sigframe_t *)(vmm_phys_to_virt(phys) + off);
        frame->rax    = regs->rax;
        frame->rbx    = regs->rbx;
        frame->rcx    = regs->rcx;
        frame->rdx    = regs->rdx;
        frame->rsi    = regs->rsi;
        frame->rdi    = regs->rdi;
        frame->rbp    = regs->rbp;
        frame->rsp    = regs->rsp;
        frame->r8     = regs->r8;
        frame->r9     = regs->r9;
        frame->r10    = regs->r10;
        frame->r11    = regs->r11;
        frame->r12    = regs->r12;
        frame->r13    = regs->r13;
        frame->r14    = regs->r14;
        frame->r15    = regs->r15;
        frame->rip    = regs->rip;
        frame->rflags = regs->rflags;
        frame->sig_mask = t->sig_mask;
        frame->signo  = (uint64_t)sig;

        /* Block signals during handler execution */
        if (!(sa_flags & SA_NODEFER))
            t->sig_mask |= (1ULL << sig);
        t->sig_mask |= sa_mask;

        /* Redirect execution to handler */
        regs->rdi = (uint64_t)sig;    /* first arg = signal number */
        regs->rip = handler_addr;
        regs->rsp = user_sp;

        /* Set return address: if SA_RESTORER is set, the restorer trampoline
         * will call rt_sigreturn. Otherwise push sigreturn syscall on stack. */
        if ((sa_flags & SA_RESTORER) && restorer_addr) {
            /* Push restorer as return address onto the user stack */
            user_sp -= 8;
            pte = vmm_get_pte(t->cr3, user_sp);
            if (pte) {
                uintptr_t rp = (*pte) & VMM_PTE_ADDR_MASK;
                uintptr_t ro = user_sp & (PAGE_SIZE - 1);
                *(uint64_t *)(vmm_phys_to_virt(rp) + ro) = restorer_addr;
            }
            regs->rsp = user_sp;
        }

        /* If SA_RESETHAND, reset to SIG_DFL after delivery */
        if ((sa_flags & SA_RESETHAND) && sa) {
            sa->sa_handler = 0;
        }

        return true; /* delivered one signal */
    }

    return false;
}

/* ── sys_rt_sigaction ─────────────────────────────────────────────────────── */
int64_t sys_rt_sigaction(int sig, const kernel_sigaction_t *act,
                         kernel_sigaction_t *oldact, uint64_t sigsetsize) {
    (void)sigsetsize;
    if (sig <= 0 || sig >= NSIGS) return -EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;

    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    /* Lazy-allocate sigactions table */
    if (!cur->sigactions) {
        cur->sigactions = sigaction_table_alloc();
        if (!cur->sigactions) return -ENOMEM;
    }

    if (oldact)
        *oldact = cur->sigactions[sig];

    if (act) {
        cur->sigactions[sig] = *act;
        /* Also update legacy sig_handlers for compatibility */
        if (cur->sig_handlers) {
            cur->sig_handlers[sig] = (sig_handler_t)(uintptr_t)act->sa_handler;
        }
    }

    return 0;
}

/* ── sys_rt_sigprocmask ───────────────────────────────────────────────────── */
int64_t sys_rt_sigprocmask(int how, const uint64_t *set,
                           uint64_t *oldset, uint64_t sigsetsize) {
    (void)sigsetsize;
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    if (oldset)
        *oldset = cur->sig_mask;

    if (set) {
        uint64_t s = *set;
        /* Never allow blocking SIGKILL or SIGSTOP */
        s &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

        switch (how) {
            case SIG_BLOCK:   cur->sig_mask |= s;  break;
            case SIG_UNBLOCK: cur->sig_mask &= ~s;  break;
            case SIG_SETMASK: cur->sig_mask = s;     break;
            default: return -EINVAL;
        }
    }

    return 0;
}

/* ── sys_rt_sigreturn ─────────────────────────────────────────────────────── */
int64_t sys_rt_sigreturn(void *regs_ptr) {
    task_t *cur = sched_current();
    if (!cur || !regs_ptr) return -EFAULT;

    cpu_regs_t *regs = (cpu_regs_t *)regs_ptr;

    /* The sigframe is at the current RSP (pushed by signal delivery) */
    uintptr_t frame_addr = regs->rsp;

    /* Read sigframe from user stack */
    uint64_t *pte = vmm_get_pte(cur->cr3, frame_addr);
    if (!pte) return -EFAULT;

    uintptr_t phys = (*pte) & VMM_PTE_ADDR_MASK;
    uintptr_t off  = frame_addr & (PAGE_SIZE - 1);
    sigframe_t *frame = (sigframe_t *)(vmm_phys_to_virt(phys) + off);

    /* Restore registers */
    regs->rax    = frame->rax;
    regs->rbx    = frame->rbx;
    regs->rcx    = frame->rcx;
    regs->rdx    = frame->rdx;
    regs->rsi    = frame->rsi;
    regs->rdi    = frame->rdi;
    regs->rbp    = frame->rbp;
    regs->rsp    = frame->rsp;
    regs->r8     = frame->r8;
    regs->r9     = frame->r9;
    regs->r10    = frame->r10;
    regs->r11    = frame->r11;
    regs->r12    = frame->r12;
    regs->r13    = frame->r13;
    regs->r14    = frame->r14;
    regs->r15    = frame->r15;
    regs->rip    = frame->rip;
    regs->rflags = frame->rflags;

    /* Restore signal mask */
    cur->sig_mask = frame->sig_mask;

    return regs->rax;  /* return original rax so the restored syscall result is correct */
}
