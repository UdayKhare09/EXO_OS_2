#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ── Signal numbers (POSIX-compatible subset) ────────────────────────────── */
#define NSIGS     32

#define SIGHUP     1   /* hangup                       */
#define SIGINT     2   /* interrupt (Ctrl-C)           */
#define SIGQUIT    3   /* quit                         */
#define SIGILL     4   /* illegal instruction          */
#define SIGTRAP    5   /* trace trap                   */
#define SIGABRT    6   /* abort                        */
#define SIGBUS     7   /* bus error                    */
#define SIGFPE     8   /* floating-point exception     */
#define SIGKILL    9   /* kill (uncatchable)           */
#define SIGUSR1   10   /* user-defined signal 1        */
#define SIGSEGV   11   /* segmentation fault            */
#define SIGUSR2   12   /* user-defined signal 2        */
#define SIGPIPE   13   /* broken pipe                  */
#define SIGALRM   14   /* alarm clock                  */
#define SIGTERM   15   /* termination request          */
#define SIGCHLD   17   /* child stopped or exited      */
#define SIGCONT   18   /* continue                     */
#define SIGSTOP   19   /* stop (uncatchable)           */
#define SIGTSTP   20   /* keyboard stop                */
#define SIGXCPU   24   /* CPU time limit exceeded      */
#define SIGXFSZ   25   /* file size limit exceeded     */

/* SA flags */
#define SA_RESTORER   0x04000000
#define SA_SIGINFO    0x00000004
#define SA_RESTART    0x10000000
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000
#define SA_NOCLDSTOP  0x00000001
#define SA_NOCLDWAIT  0x00000002

/* sigprocmask how */
#define SIG_BLOCK     0
#define SIG_UNBLOCK   1
#define SIG_SETMASK   2

typedef void (*sig_handler_t)(int signo);

/* Special values for sig_handler_t */
#define SIG_DFL ((sig_handler_t)0)   /* default action (ignore in kernel) */
#define SIG_IGN ((sig_handler_t)1)   /* explicitly ignored                */

/* Linux x86_64 sigaction structure (as used by rt_sigaction) */
typedef struct {
    uint64_t  sa_handler;    /* handler address (or SIG_DFL/SIG_IGN)  */
    uint64_t  sa_flags;      /* SA_SIGINFO, SA_RESTORER, etc.         */
    uint64_t  sa_restorer;   /* address of sigreturn trampoline       */
    uint64_t  sa_mask;       /* signals to block during handler       */
} kernel_sigaction_t;

/* Signal frame pushed onto user stack for signal delivery.
 * The sigreturn syscall restores registers from this frame. */
typedef struct {
    uint64_t  rax, rbx, rcx, rdx;
    uint64_t  rsi, rdi, rbp, rsp;
    uint64_t  r8,  r9,  r10, r11;
    uint64_t  r12, r13, r14, r15;
    uint64_t  rip;
    uint64_t  rflags;
    uint64_t  sig_mask;      /* original signal mask to restore       */
    uint64_t  signo;         /* signal number (for debugging)         */
} sigframe_t;

struct task;   /* forward declaration */

/* ── Lifecycle ───────────────────────────────────────────────────────────── */
sig_handler_t *signal_table_alloc(void);
void           signal_table_free(sig_handler_t *tbl);

/* Allocate/free sigaction table (NSIGS entries) */
kernel_sigaction_t *sigaction_table_alloc(void);
void                sigaction_table_free(kernel_sigaction_t *tbl);

/* ── Handler management ──────────────────────────────────────────────────── */
void          signal_set(struct task *t, int sig, sig_handler_t handler);
sig_handler_t signal_get(struct task *t, int sig);

/* ── Delivery ────────────────────────────────────────────────────────────── */
void signal_send(struct task *t, int sig);

/* Dispatch pending signals for task `t`. For kernel tasks, handlers are
 * called directly. For user tasks, this is a no-op (delivery happens
 * in the syscall return path via signal_deliver_user). */
void signal_dispatch(struct task *t);

/* Deliver a signal to a user-mode task by pushing a sigframe onto the
 * user stack and redirecting RIP to the handler. `regs` is the saved
 * cpu_regs_t from the syscall/interrupt path. Returns true if delivered. */
bool signal_deliver_user(struct task *t, void *regs);

/* ── Syscall implementations ─────────────────────────────────────────────── */
int64_t sys_rt_sigaction(int sig, const kernel_sigaction_t *act,
                         kernel_sigaction_t *oldact, uint64_t sigsetsize);
int64_t sys_rt_sigprocmask(int how, const uint64_t *set,
                           uint64_t *oldset, uint64_t sigsetsize);
int64_t sys_rt_sigreturn(void *regs);
