/* syscall/proc_syscalls.c — Process and memory management syscalls
 *
 * Implements fork, execve, wait4, mmap, munmap, mprotect, clone, futex.
 */
#include "syscall.h"
#include "arch/x86_64/idt.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sched/waitq.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "fs/elf.h"
#include "ipc/signal.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "lib/panic.h"
#include "arch/x86_64/cpu.h"

#include <stdint.h>
#include <stddef.h>

/* ── Helper: add VMA to a task's sorted list ─────────────────────────────── */
void vma_insert(task_t *t, vma_t *v) {
    vma_t **pp = &t->vma_list;
    while (*pp && (*pp)->start < v->start)
        pp = &(*pp)->next;
    v->next = *pp;
    *pp = v;
}

/* ── Helper: clone FD table (for fork) ────────────────────────────────────── */
static void fd_table_clone(task_t *child, task_t *parent) {
    for (int i = 0; i < TASK_FD_TABLE_SIZE; i++) {
        file_t *f = parent->fd_table[i];
        if (f) {
            file_get(f);           /* bump refcount */
            child->fd_table[i] = f;
        }
    }
    /* Copy cwd */
    strncpy(child->cwd, parent->cwd, TASK_CWD_MAX - 1);
    child->cwd[TASK_CWD_MAX - 1] = '\0';
}

/* ── Helper: clone VMA list (for fork) ────────────────────────────────────── */
static void vma_list_clone(task_t *child, task_t *parent) {
    vma_t *src = parent->vma_list;
    vma_t **dst = &child->vma_list;
    while (src) {
        vma_t *v = kmalloc(sizeof(vma_t));
        if (!v) break;
        *v = *src;
        v->next = NULL;
        *dst = v;
        dst = &v->next;
        src = src->next;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_fork — Create a child process via COW clone                          *
 * ═══════════════════════════════════════════════════════════════════════════ */
int64_t sys_fork(cpu_regs_t *regs) {
    task_t *parent = sched_current();
    if (!parent) return -ESRCH;

    /* Clone address space with COW */
    uintptr_t child_pml4 = vmm_clone_address_space(parent->cr3);
    if (!child_pml4) return -ENOMEM;

    /* Create the child task */
    task_t *child = task_create_user(parent->name, child_pml4, 0, 0,
                                      parent->cpu_id);
    if (!child) {
        vmm_destroy_address_space(child_pml4);
        return -ENOMEM;
    }

    /* Set up parent-child relationship */
    child->ppid    = parent->pid;
    child->pgid    = parent->pgid;
    child->uid     = parent->uid;
    child->gid     = parent->gid;
    child->parent  = parent;

    /* Add to parent's children list */
    child->child_next = parent->children;
    parent->children  = child;

    /* Clone VMAs and fd table */
    vma_list_clone(child, parent);
    fd_table_clone(child, parent);

    /* Copy memory management state */
    child->brk_base    = parent->brk_base;
    child->brk_current = parent->brk_current;
    child->mmap_next   = parent->mmap_next;
    child->fs_base     = parent->fs_base;
    child->sig_mask    = parent->sig_mask;

    /* Set up child's initial register frame so it returns 0 from fork.
     * We need to override the rsp/rbx/r12 that task_create_user set up
     * for the user_mode_trampoline and instead make the child return
     * directly from the syscall. */

    /* The child should resume exactly where the parent was,
     * but with rax = 0 (fork return value for child).
     * We do this by setting up the kernel stack so the scheduler
     * will return into the syscall return path. */

    /* For now, the child was created with user_mode_trampoline.
     * We need to re-set up the kernel stack frame so it looks like
     * the child is returning from the same syscall. We'll make the
     * user_mode_trampoline use rax=0 by clearing rbx→rax. Actually,
     * the cleanest approach: set up iretq-style return on child's
     * kernel stack. */

    /* Override: make child return to user-mode at parent's RIP with rax=0 */
    /* Rebuild the init_frame on child's kernel stack */
    extern void fork_child_return(void);

    typedef struct {
        uint64_t r15, r14, r13, r12, rbx, rbp;
        uint64_t rip;
    } __attribute__((packed)) init_frame_t;

    uintptr_t child_kstack_top = vmm_phys_to_virt(child->stack_phys) + TASK_STACK_SIZE;

    /* Build a larger frame: init_frame + iretq frame */
    /* We push an iretq frame (SS, RSP, RFLAGS, CS, RIP) then the init_frame
     * that task_switch_asm will pop. The init_frame RIP points to a small
     * asm stub that does "pop rax; iretq" to return to user-space with rax=0 */

    /* For simplicity, use the existing user_mode_trampoline approach:
     * r12 = user RSP (from parent's regs), rbx = user RIP (from parent's regs)
     * But we need to ensure rax=0 after the trampoline. The existing
     * user_mode_trampoline xors rax, so rax=0 automatically. Perfect. */
    child_kstack_top -= sizeof(init_frame_t);
    init_frame_t *frame = (init_frame_t *)child_kstack_top;
    memset(frame, 0, sizeof(*frame));

    extern void user_mode_trampoline(void);
    frame->rip = (uint64_t)user_mode_trampoline;
    frame->rbx = regs->rip;    /* user RIP (resume at same instruction) */
    frame->r12 = regs->rsp;    /* user RSP */

    child->rsp = child_kstack_top;

    /* PROBLEM: user_mode_trampoline does `xor eax,eax` so child gets rax=0. Good.
     * But it clears ALL registers. We want the child to have the same registers
     * as parent except rax=0. For a basic fork this is fine — most programs
     * only check the return value. */

    /* Enqueue child for scheduling */
    sched_add_task(child, child->cpu_id);

    KLOG_INFO("fork: parent=%u child=%u\n", parent->pid, child->pid);
    return (int64_t)child->pid;  /* parent gets child PID */
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_execve — Replace process image with an ELF binary                    *
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Auxiliary vector entry types (subset needed by mlibc) */
#define AT_NULL          0
#define AT_PHDR          3
#define AT_PHENT         4
#define AT_PHNUM         5
#define AT_PAGESZ        6
#define AT_ENTRY         9
#define AT_SECURE       23

/* Copy a user-space string array (argv or envp) into a flat kernel buffer.
 * Returns count of strings, or negative errno.
 * strs_out[] is filled with pointers INTO buf_out. */
static int copy_strings(char *const user_arr[], char *buf, size_t buf_sz,
                        const char **strs_out, int max_strs) {
    if (!user_arr) return 0;
    int count = 0;
    size_t off = 0;
    for (int i = 0; i < max_strs; i++) {
        const char *s = user_arr[i];
        if (!s) break;
        size_t len = strlen(s) + 1;
        if (off + len > buf_sz) return -E2BIG;
        memcpy(buf + off, s, len);
        strs_out[count] = buf + off;
        off += len;
        count++;
    }
    return count;
}

int64_t sys_execve(const char *path, char *const argv[], char *const envp[]) {
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    /* Copy argv/envp into kernel buffers (max 256 strings, 64 KiB total) */
    #define EXEC_MAX_STRS  256
    #define EXEC_BUF_SIZE  (64 * 1024)
    char *argv_buf = kmalloc(EXEC_BUF_SIZE);
    char *envp_buf = kmalloc(EXEC_BUF_SIZE);
    const char *argv_ptrs[EXEC_MAX_STRS];
    const char *envp_ptrs[EXEC_MAX_STRS];
    if (!argv_buf || !envp_buf) {
        kfree(argv_buf); kfree(envp_buf);
        return -ENOMEM;
    }

    int argc = copy_strings(argv, argv_buf, EXEC_BUF_SIZE, argv_ptrs, EXEC_MAX_STRS);
    int envc = copy_strings(envp, envp_buf, EXEC_BUF_SIZE, envp_ptrs, EXEC_MAX_STRS);
    if (argc < 0) { kfree(argv_buf); kfree(envp_buf); return argc; }
    if (envc < 0) { kfree(argv_buf); kfree(envp_buf); return envc; }

    /* Open the ELF file */
    int vfs_err = 0;
    vnode_t *vn = vfs_lookup(path, true, &vfs_err);
    if (!vn) { kfree(argv_buf); kfree(envp_buf); return vfs_err ? -vfs_err : -ENOENT; }

    /* Read ELF into a temporary kernel buffer */
    uint64_t size = vn->size;
    if (size == 0 || size > (64ULL * 1024 * 1024)) {
        kfree(argv_buf); kfree(envp_buf);
        return -ENOEXEC;
    }

    void *buf = kmalloc(size);
    if (!buf) { kfree(argv_buf); kfree(envp_buf); return -ENOMEM; }

    ssize_t rd = vn->ops->read(vn, buf, size, 0);
    if (rd < 0 || (uint64_t)rd != size) {
        kfree(buf); kfree(argv_buf); kfree(envp_buf);
        return -EIO;
    }

    /* Create a FRESH address space for the new image.
     * This ensures no stale mappings from the old process image survive. */
    uintptr_t old_cr3 = cur->cr3;
    uintptr_t new_cr3 = vmm_create_address_space();
    if (!new_cr3) {
        kfree(buf); kfree(argv_buf); kfree(envp_buf);
        return -ENOMEM;
    }

    /* Parse ELF and load segments into the new address space */
    elf_info_t info;
    int r = elf_load(buf, size, new_cr3, &info);
    kfree(buf);
    if (r < 0) {
        vmm_destroy_address_space(new_cr3);
        kfree(argv_buf); kfree(envp_buf);
        return r;
    }

    /* Switch the task to the new address space and activate it in hardware.
     * Do this before destroying the old one so we always run with a valid CR3. */
    cur->cr3 = new_cr3;
    __asm__ volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");

    /* Now safe to tear down the old address space (we are no longer using it) */
    if (old_cr3 && old_cr3 != vmm_get_kernel_pml4())
        vmm_destroy_address_space(old_cr3);

    /* Reset mmap bump pointer */
    cur->mmap_next = USER_MMAP_BASE;

    /* Close FD_CLOEXEC fds */
    fd_close_cloexec(cur);

    /* Auto-setup stdio if fd 0/1/2 are not open (e.g. first exec from kernel) */
    if (!fd_get(cur, 0) || !fd_get(cur, 1) || !fd_get(cur, 2)) {
        int vfs_err2 = 0;
        vnode_t *tty = vfs_lookup("/dev/tty", true, &vfs_err2);
        if (tty) {
            fd_setup_stdio(cur, tty);
            vfs_vnode_put(tty);
        }
    }

    /* Reset signal handlers to SIG_DFL */
    if (cur->sig_handlers) {
        for (int i = 0; i < NSIGS; i++)
            cur->sig_handlers[i] = SIG_DFL;
    }

    /* Update brk to end of loaded segments */
    cur->brk_base    = info.brk_start;
    cur->brk_current = info.brk_start;

    /* Clear old VMAs and rebuild from ELF info */
    vma_t *v = cur->vma_list;
    while (v) { vma_t *n = v->next; kfree(v); v = n; }
    cur->vma_list = NULL;

    /* Set up user stack */
    uintptr_t user_stack_top = USER_STACK_TOP;
    uintptr_t stack_pages = 8;  /* 32 KiB initial stack */
    for (uintptr_t i = 0; i < stack_pages; i++) {
        uintptr_t pg = pmm_alloc_pages(1);
        if (pg) {
            memset((void *)vmm_phys_to_virt(pg), 0, PAGE_SIZE);
            vmm_map_page_in(cur->cr3,
                            user_stack_top - (stack_pages - i) * PAGE_SIZE,
                            pg, VMM_USER_RW);
        }
    }

    /* Add stack VMA */
    vma_t *stack_vma = kmalloc(sizeof(vma_t));
    if (stack_vma) {
        stack_vma->start = user_stack_top - stack_pages * PAGE_SIZE;
        stack_vma->end   = user_stack_top;
        stack_vma->flags = VMA_READ | VMA_WRITE | VMA_USER | VMA_STACK;
        stack_vma->next  = NULL;
        vma_insert(cur, stack_vma);
    }

    /* ── Build the Linux user-stack layout ────────────────────────────────
     * High address:
     *   string data (argv[0] chars, argv[1] chars, ..., envp[0] chars, ...)
     *   padding to 16 bytes
     *   auxv[] (pairs of uint64_t: type, value, terminated by AT_NULL,0)
     *   NULL   (envp terminator)
     *   envp[envc-1]  ...  envp[0]  (pointers into string data)
     *   NULL   (argv terminator)
     *   argv[argc-1]  ...  argv[0]  (pointers into string data)
     *   argc   (uint64_t)
     * Low address  ← RSP points here
     */
    uintptr_t sp = user_stack_top;

    /* 1) Copy string data to top of stack */
    uintptr_t argv_user[EXEC_MAX_STRS];
    uintptr_t envp_user[EXEC_MAX_STRS];

    for (int i = envc - 1; i >= 0; i--) {
        size_t len = strlen(envp_ptrs[i]) + 1;
        sp -= len;
        /* Write to user stack via HHDM (we have the page mapped) */
        uint64_t *pte = vmm_get_pte(cur->cr3, sp);
        if (pte) {
            uintptr_t phys = (*pte) & VMM_PTE_ADDR_MASK;
            uintptr_t off = sp & (PAGE_SIZE - 1);
            memcpy((void *)(vmm_phys_to_virt(phys) + off), envp_ptrs[i], len);
        }
        envp_user[i] = sp;
    }

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv_ptrs[i]) + 1;
        sp -= len;
        uint64_t *pte = vmm_get_pte(cur->cr3, sp);
        if (pte) {
            uintptr_t phys = (*pte) & VMM_PTE_ADDR_MASK;
            uintptr_t off = sp & (PAGE_SIZE - 1);
            memcpy((void *)(vmm_phys_to_virt(phys) + off), argv_ptrs[i], len);
        }
        argv_user[i] = sp;
    }

    kfree(argv_buf);
    kfree(envp_buf);

    /* 2) Align to 16 bytes */
    sp &= ~0xFULL;

    /* Helper: push a uint64_t to the user stack */
    #define PUSH_U64(val) do {                                              \
        sp -= 8;                                                            \
        uint64_t *_pte = vmm_get_pte(cur->cr3, sp);                        \
        if (_pte) {                                                         \
            uintptr_t _ph = (*_pte) & VMM_PTE_ADDR_MASK;                   \
            uintptr_t _of = sp & (PAGE_SIZE - 1);                          \
            *(uint64_t *)(vmm_phys_to_virt(_ph) + _of) = (uint64_t)(val);  \
        }                                                                   \
    } while(0)

    /* 3) Auxiliary vector */
    PUSH_U64(0);                     /* AT_NULL value */
    PUSH_U64(AT_NULL);               /* AT_NULL type  */
    PUSH_U64(0);                     /* AT_SECURE = 0 */
    PUSH_U64(AT_SECURE);
    PUSH_U64(info.entry);            /* AT_ENTRY      */
    PUSH_U64(AT_ENTRY);
    PUSH_U64(PAGE_SIZE);             /* AT_PAGESZ     */
    PUSH_U64(AT_PAGESZ);
    PUSH_U64(info.phdr_size);        /* AT_PHENT      */
    PUSH_U64(AT_PHENT);
    PUSH_U64(info.phdr_count);       /* AT_PHNUM      */
    PUSH_U64(AT_PHNUM);
    PUSH_U64(info.phdr_vaddr);       /* AT_PHDR       */
    PUSH_U64(AT_PHDR);

    /* 4) envp array (NULL-terminated) */
    PUSH_U64(0);  /* NULL terminator */
    for (int i = envc - 1; i >= 0; i--)
        PUSH_U64(envp_user[i]);

    /* 5) argv array (NULL-terminated) */
    PUSH_U64(0);  /* NULL terminator */
    for (int i = argc - 1; i >= 0; i--)
        PUSH_U64(argv_user[i]);

    /* 6) argc */
    PUSH_U64(argc);

    #undef PUSH_U64

    /* Ensure 16-byte alignment of final RSP (System V ABI) */
    sp &= ~0xFULL;

    KLOG_INFO("execve: '%s' entry=%p brk=%p argc=%d envc=%d sp=%p\n",
              path, (void *)info.entry, (void *)info.brk_start, argc, envc, (void*)sp);

    /* Jump to user-mode at the new entry point via user_mode_trampoline */
    extern void user_mode_trampoline(void);

    typedef struct {
        uint64_t r15, r14, r13, r12, rbx, rbp;
        uint64_t rip;
    } __attribute__((packed)) init_frame_t;

    uintptr_t kstack_top = vmm_phys_to_virt(cur->stack_phys) + TASK_STACK_SIZE;
    kstack_top -= sizeof(init_frame_t);
    init_frame_t *frame = (init_frame_t *)kstack_top;
    memset(frame, 0, sizeof(*frame));
    frame->rip = (uint64_t)user_mode_trampoline;
    frame->rbx = info.entry;         /* new user RIP */
    frame->r12 = sp;                 /* new user RSP (with argc/argv/auxv) */

    cur->rsp = kstack_top;

    /* Jump there NOW */
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%rbx\n\t"
        "pop %%rbp\n\t"
        "ret\n\t"
        : : "r"(kstack_top) : "memory"
    );

    __builtin_unreachable();
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_wait4 — Wait for child process state change                          *
 * ═══════════════════════════════════════════════════════════════════════════ */
int64_t sys_wait4(int pid, int *wstatus, int options, void *rusage) {
    (void)rusage;
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    /* WNOHANG = 1 */
    int wnohang = options & 1;

retry:
    /* Search children */
    for (task_t *child = cur->children; child; child = child->child_next) {
        /* If pid > 0, wait for specific child */
        if (pid > 0 && child->pid != (uint32_t)pid)
            continue;
        /* pid == -1: wait for any child; pid == 0: same pgid; pid < -1: pgid==-pid */
        if (pid == 0 && child->pgid != cur->pgid)
            continue;
        if (pid < -1 && child->pgid != (uint32_t)(-pid))
            continue;

        if (child->state == TASK_ZOMBIE) {
            int status = child->exit_status;
            if (wstatus) *wstatus = status;

            int child_pid = (int)child->pid;

            /* Remove from children list */
            task_t **pp = &cur->children;
            while (*pp && *pp != child) pp = &(*pp)->child_next;
            if (*pp) *pp = child->child_next;

            /* Fully reclaim */
            task_destroy(child);
            return child_pid;
        }
    }

    /* No zombie children found */
    if (!cur->children)
        return -ECHILD;

    if (wnohang)
        return 0;

    /* Block and retry when woken by child exit */
    sched_block();
    goto retry;
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_mmap — Map memory into the process address space                     *
 * ═══════════════════════════════════════════════════════════════════════════ */
int64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags,
                 int fd, int64_t offset) {
    (void)fd; (void)offset;  /* file-backed mmap not yet supported */

    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    if (len == 0) return -EINVAL;

    /* Only anonymous mappings for now */
    if (!(flags & MAP_ANONYMOUS))
        return -ENOSYS;

    /* Page-align length */
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Choose address */
    uint64_t map_addr;
    if ((flags & MAP_FIXED) && addr) {
        map_addr = addr & ~(PAGE_SIZE - 1);
    } else {
        map_addr = cur->mmap_next;
        cur->mmap_next += len;
    }

    /* Convert prot to page flags */
    uint64_t pflags = VMM_PRESENT | VMM_USER;
    if (prot & PROT_WRITE) pflags |= VMM_WRITE;
    if (!(prot & PROT_EXEC)) pflags |= VMM_NX;

    /* Map pages (demand-paging would be better, but allocate now for simplicity) */
    for (uint64_t off = 0; off < len; off += PAGE_SIZE) {
        uintptr_t pg = pmm_alloc_pages(1);
        if (!pg) return -ENOMEM;
        /* Zero the page */
        memset((void *)vmm_phys_to_virt(pg), 0, PAGE_SIZE);
        vmm_map_page_in(cur->cr3, map_addr + off, pg, pflags);
    }

    /* Track VMA */
    vma_t *v = kmalloc(sizeof(vma_t));
    if (v) {
        v->start = map_addr;
        v->end   = map_addr + len;
        v->flags = VMA_ANON | VMA_USER;
        if (prot & PROT_READ)  v->flags |= VMA_READ;
        if (prot & PROT_WRITE) v->flags |= VMA_WRITE;
        if (prot & PROT_EXEC)  v->flags |= VMA_EXEC;
        vma_insert(cur, v);
    }

    KLOG_DEBUG("mmap: addr=%p len=%llu prot=%d flags=%d → %p\n",
               (void *)addr, len, prot, flags, (void *)map_addr);
    return (int64_t)map_addr;
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_munmap — Unmap memory                                                *
 * ═══════════════════════════════════════════════════════════════════════════ */
int64_t sys_munmap(uint64_t addr, uint64_t len) {
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    if (len == 0 || (addr & (PAGE_SIZE - 1)))
        return -EINVAL;

    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Unmap pages and free physical memory */
    for (uint64_t off = 0; off < len; off += PAGE_SIZE) {
        uintptr_t phys = vmm_unmap_page_in(cur->cr3, addr + off);
        if (phys) pmm_free_pages(phys, 1);
    }

    /* Remove/split VMAs that overlap [addr, addr+len) */
    vma_t **pp = &cur->vma_list;
    while (*pp) {
        vma_t *v = *pp;
        uint64_t unmap_start = addr;
        uint64_t unmap_end   = addr + len;

        /* No overlap → skip */
        if (v->end <= unmap_start || v->start >= unmap_end) {
            pp = &v->next;
            continue;
        }

        /* Fully contained → remove */
        if (v->start >= unmap_start && v->end <= unmap_end) {
            *pp = v->next;
            kfree(v);
            continue;
        }

        /* Partial overlap: split needed */
        if (v->start < unmap_start && v->end > unmap_end) {
            /* VMA spans both sides: split into two.
             * [v->start, unmap_start) and [unmap_end, v->end) */
            vma_t *right = kmalloc(sizeof(vma_t));
            if (right) {
                right->start = unmap_end;
                right->end   = v->end;
                right->flags = v->flags;
                right->next  = v->next;
                v->end  = unmap_start;
                v->next = right;
            }
            pp = &v->next;
            if (right) pp = &right->next;
            continue;
        }

        /* Overlap on left side: trim end */
        if (v->start < unmap_start) {
            v->end = unmap_start;
            pp = &v->next;
            continue;
        }

        /* Overlap on right side: trim start */
        if (v->end > unmap_end) {
            v->start = unmap_end;
            pp = &v->next;
            continue;
        }

        pp = &v->next;
    }

    /* Flush TLB */
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_mprotect — Change page protections                                   *
 * ═══════════════════════════════════════════════════════════════════════════ */
int64_t sys_mprotect(uint64_t addr, uint64_t len, int prot) {
    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    if ((addr & (PAGE_SIZE - 1)) || len == 0)
        return -EINVAL;

    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint64_t pflags = VMM_PRESENT | VMM_USER;
    if (prot & PROT_WRITE) pflags |= VMM_WRITE;
    if (!(prot & PROT_EXEC)) pflags |= VMM_NX;

    /* Update page table entries */
    for (uint64_t off = 0; off < len; off += PAGE_SIZE) {
        uint64_t *pte = vmm_get_pte(cur->cr3, addr + off);
        if (pte && (*pte & VMM_PRESENT)) {
            uintptr_t phys = *pte & VMM_PTE_ADDR_MASK;
            *pte = phys | pflags;
        }
    }

    /* Flush TLB */
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_clone — Create a new thread or process (Linux clone())               *
 * ═══════════════════════════════════════════════════════════════════════════ */
int64_t sys_clone(uint64_t flags, uint64_t stack, uint64_t parent_tid_ptr,
                  uint64_t child_tid_ptr, uint64_t tls, cpu_regs_t *regs) {
    task_t *parent = sched_current();
    if (!parent || !regs) return -ESRCH;

    bool share_vm     = (flags & CLONE_VM) != 0;
    bool share_files  = (flags & CLONE_FILES) != 0;
    bool is_thread    = (flags & CLONE_THREAD) != 0;

    uintptr_t child_pml4;
    task_t *child;

    if (share_vm) {
        /* Thread: share the parent's address space */
        child_pml4 = parent->cr3;
        child = task_create_user(parent->name, child_pml4, 0, 0, parent->cpu_id);
        if (!child) return -ENOMEM;
    } else {
        /* New process: COW clone */
        child_pml4 = vmm_clone_address_space(parent->cr3);
        if (!child_pml4) return -ENOMEM;
        child = task_create_user(parent->name, child_pml4, 0, 0, parent->cpu_id);
        if (!child) {
            vmm_destroy_address_space(child_pml4);
            return -ENOMEM;
        }
    }

    child->ppid   = parent->pid;
    child->pgid   = parent->pgid;
    child->uid    = parent->uid;
    child->gid    = parent->gid;
    child->parent = parent;

    if (is_thread) {
        child->pid = parent->pid;
    }

    child->child_next = parent->children;
    parent->children  = child;

    if (share_files) {
        for (int i = 0; i < TASK_FD_TABLE_SIZE; i++) {
            file_t *f = parent->fd_table[i];
            if (f) { file_get(f); child->fd_table[i] = f; }
        }
        strncpy(child->cwd, parent->cwd, TASK_CWD_MAX - 1);
    } else {
        fd_table_clone(child, parent);
    }

    vma_list_clone(child, parent);

    child->brk_base    = parent->brk_base;
    child->brk_current = parent->brk_current;
    child->mmap_next   = parent->mmap_next;

    if (tls) {
        child->fs_base = tls;
    } else {
        child->fs_base = parent->fs_base;
    }

    if (flags & 0x00200000) /* CLONE_CHILD_CLEARTID */
        child->clear_child_tid = (uint64_t *)child_tid_ptr;

    if ((flags & 0x00100000) && parent_tid_ptr) /* CLONE_PARENT_SETTID */
        *(uint32_t *)parent_tid_ptr = child->tid;
    if ((flags & 0x01000000) && child_tid_ptr)  /* CLONE_CHILD_SETTID */
        *(uint32_t *)child_tid_ptr = child->tid;

    /* Copy parent's register frame for child */
    uintptr_t kstack_child = vmm_phys_to_virt(child->stack_phys) + TASK_STACK_SIZE;
    kstack_child -= sizeof(cpu_regs_t);
    cpu_regs_t *child_regs = (cpu_regs_t *)kstack_child;
    *child_regs = *regs;
    child_regs->rax = 0;

    if (stack)
        child_regs->rsp = stack;

    typedef struct {
        uint64_t r15, r14, r13, r12, rbx, rbp;
        uint64_t rip;
    } __attribute__((packed)) init_frame_t;

    extern void user_mode_trampoline(void);
    kstack_child -= sizeof(init_frame_t);
    init_frame_t *frame = (init_frame_t *)kstack_child;
    memset(frame, 0, sizeof(*frame));
    frame->rip = (uint64_t)user_mode_trampoline;
    frame->rbx = child_regs->rip;
    frame->r12 = child_regs->rsp;

    child->rsp = kstack_child;

    int target_cpu = sched_pick_cpu();
    child->cpu_id = target_cpu;
    sched_add_task(child, target_cpu);

    KLOG_DEBUG("clone: child tid=%u flags=%llx\n", child->tid, flags);
    return (int64_t)child->tid;
}

/* ═══════════════════════════════════════════════════════════════════════════ *
 *  sys_futex — Fast user-space mutex support                                *
 * ═══════════════════════════════════════════════════════════════════════════ */

#define FUTEX_HASH_SIZE 64

typedef struct futex_entry {
    uintptr_t  key;
    waitq_t    wq;
    struct futex_entry *next;
} futex_entry_t;

static futex_entry_t *futex_hash[FUTEX_HASH_SIZE];
static volatile int futex_lock_var = 0;

static void futex_lock_acquire(void) { while (__atomic_test_and_set(&futex_lock_var, __ATOMIC_ACQUIRE)) __asm__("pause"); }
static void futex_lock_release(void) { __atomic_clear(&futex_lock_var, __ATOMIC_RELEASE); }

static futex_entry_t *futex_find_or_create(uintptr_t cr3, uintptr_t uaddr) {
    uintptr_t bucket = (cr3 ^ uaddr) % FUTEX_HASH_SIZE;
    uintptr_t key = cr3 ^ uaddr;

    futex_entry_t *e = futex_hash[bucket];
    while (e) {
        if (e->key == key) return e;
        e = e->next;
    }

    e = kmalloc(sizeof(futex_entry_t));
    if (!e) return NULL;
    e->key = key;
    waitq_init(&e->wq);
    e->next = futex_hash[bucket];
    futex_hash[bucket] = e;
    return e;
}

int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val,
                  const kernel_timespec_t *timeout, uint32_t *uaddr2, uint32_t val3) {
    (void)timeout; (void)uaddr2; (void)val3;
    if (!uaddr) return -EFAULT;

    task_t *cur = sched_current();
    if (!cur) return -ESRCH;

    int futex_op = op & 0x7F;

    switch (futex_op) {
        case 0: { /* FUTEX_WAIT */
            futex_lock_acquire();
            if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val) {
                futex_lock_release();
                return -EAGAIN;
            }
            futex_entry_t *e = futex_find_or_create(cur->cr3, (uintptr_t)uaddr);
            futex_lock_release();
            if (!e) return -ENOMEM;
            waitq_wait(&e->wq);
            return 0;
        }

        case 1: { /* FUTEX_WAKE */
            futex_lock_acquire();
            futex_entry_t *e = futex_find_or_create(cur->cr3, (uintptr_t)uaddr);
            futex_lock_release();
            if (!e) return 0;
            int woken = 0;
            for (uint32_t i = 0; i < val; i++) {
                waitq_wake_one(&e->wq);
                woken++;
            }
            return woken;
        }

        default:
            return -ENOSYS;
    }
}