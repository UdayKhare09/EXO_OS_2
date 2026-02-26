/* syscall/syscall.c — INT 0x80 syscall dispatcher
 *
 * Registers vector 0x80 with DPL=3 so user-space can call INT 0x80.
 * Syscall number in rax; args in rdi, rsi, rdx, r10, r8, r9.
 * Return value written back to regs->rax.
 */
#include "syscall.h"
#include "arch/x86_64/idt.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "fs/vfs.h"
#include "fs/fd.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "lib/panic.h"
#include "mm/kmalloc.h"

#include <stdint.h>

/* ── Forward declarations to file_syscalls.c ────────────────────────────── */
int64_t sys_read(int fd, void *buf, uint64_t count);
int64_t sys_write(int fd, const void *buf, uint64_t count);
int64_t sys_open(const char *path, int flags, uint32_t mode);
int64_t sys_close(int fd);
int64_t sys_stat(const char *path, linux_stat_t *buf);
int64_t sys_fstat(int fd, linux_stat_t *buf);
int64_t sys_lstat(const char *path, linux_stat_t *buf);
int64_t sys_lseek(int fd, int64_t off, int whence);
int64_t sys_dup(int old_fd);
int64_t sys_dup2(int old_fd, int new_fd);
int64_t sys_getcwd(char *buf, uint64_t size);
int64_t sys_chdir(const char *path);
int64_t sys_mkdir(const char *path, uint32_t mode);
int64_t sys_rmdir(const char *path);
int64_t sys_unlink(const char *path);
int64_t sys_rename(const char *old, const char *newp);
int64_t sys_getdents64(int fd, void *dirp, uint64_t count);
int64_t sys_brk(uint64_t addr);

/* ── Forward declarations to net_syscalls.c ─────────────────────────────── */
struct sockaddr;
struct pollfd;
typedef uint32_t socklen_t;
int64_t sys_socket(int domain, int type, int protocol);
int64_t sys_connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int64_t sys_accept(int fd, struct sockaddr *addr, socklen_t *addrlen);
int64_t sys_sendto(int fd, const void *buf, uint64_t len, int flags,
                   const struct sockaddr *dest_addr, socklen_t addrlen);
int64_t sys_recvfrom(int fd, void *buf, uint64_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen);
int64_t sys_shutdown(int fd, int how);
int64_t sys_bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int64_t sys_listen(int fd, int backlog);
int64_t sys_getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen);
int64_t sys_getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen);
int64_t sys_setsockopt(int fd, int level, int optname,
                       const void *optval, socklen_t optlen);
int64_t sys_getsockopt(int fd, int level, int optname,
                       void *optval, socklen_t *optlen);
int64_t sys_poll(struct pollfd *fds, uint64_t nfds, int timeout);
int64_t sys_ioctl(int fd, unsigned long cmd, unsigned long arg);

/* ── Syscall table ───────────────────────────────────────────────────────── */
typedef int64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t);

static int64_t sc_read(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_read((int)a,(void*)b,c); }
static int64_t sc_write(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_write((int)a,(const void*)b,c); }
static int64_t sc_open(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_open((const char*)a,(int)b,(uint32_t)c); }
static int64_t sc_close(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)b;(void)c;(void)d;(void)e;(void)f; return sys_close((int)a); }
static int64_t sc_stat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_stat((const char*)a,(linux_stat_t*)b); }
static int64_t sc_fstat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_fstat((int)a,(linux_stat_t*)b); }
static int64_t sc_lstat(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_lstat((const char*)a,(linux_stat_t*)b); }
static int64_t sc_lseek(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_lseek((int)a,(int64_t)b,(int)c); }
static int64_t sc_brk(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)b;(void)c;(void)d;(void)e;(void)f; return sys_brk(a); }
static int64_t sc_dup(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)b;(void)c;(void)d;(void)e;(void)f; return sys_dup((int)a); }
static int64_t sc_dup2(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_dup2((int)a,(int)b); }
static int64_t sc_exit(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
    KLOG_INFO("syscall: task exit(%llu)\n", a);
    /* For now, just loop; proper implementation calls sched_exit() */
    for(;;) __asm__ volatile("hlt");
    return 0;
}
static int64_t sc_getcwd(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_getcwd((char*)a,b); }
static int64_t sc_chdir(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)b;(void)c;(void)d;(void)e;(void)f; return sys_chdir((const char*)a); }
static int64_t sc_rename(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_rename((const char*)a,(const char*)b); }
static int64_t sc_mkdir(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_mkdir((const char*)a,(uint32_t)b); }
static int64_t sc_rmdir(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)b;(void)c;(void)d;(void)e;(void)f; return sys_rmdir((const char*)a); }
static int64_t sc_unlink(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)b;(void)c;(void)d;(void)e;(void)f; return sys_unlink((const char*)a); }
static int64_t sc_getdents64(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_getdents64((int)a,(void*)b,c); }

/* ── Network syscall wrappers ────────────────────────────────────────────── */
static int64_t sc_poll(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_poll((struct pollfd*)a,b,(int)c); }
static int64_t sc_ioctl(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_ioctl((int)a,b,c); }
static int64_t sc_socket(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_socket((int)a,(int)b,(int)c); }
static int64_t sc_connect(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_connect((int)a,(const struct sockaddr*)b,(socklen_t)c); }
static int64_t sc_accept(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_accept((int)a,(struct sockaddr*)b,(socklen_t*)c); }
static int64_t sc_sendto(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { return sys_sendto((int)a,(const void*)b,c,(int)d,(const struct sockaddr*)e,(socklen_t)f); }
static int64_t sc_recvfrom(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { return sys_recvfrom((int)a,(void*)b,c,(int)d,(struct sockaddr*)e,(socklen_t*)f); }
static int64_t sc_shutdown(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_shutdown((int)a,(int)b); }
static int64_t sc_bind(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_bind((int)a,(const struct sockaddr*)b,(socklen_t)c); }
static int64_t sc_listen(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)c;(void)d;(void)e;(void)f; return sys_listen((int)a,(int)b); }
static int64_t sc_getsockname(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_getsockname((int)a,(struct sockaddr*)b,(socklen_t*)c); }
static int64_t sc_getpeername(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)d;(void)e;(void)f; return sys_getpeername((int)a,(struct sockaddr*)b,(socklen_t*)c); }
static int64_t sc_setsockopt(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)f; return sys_setsockopt((int)a,(int)b,(int)c,(const void*)d,(socklen_t)e); }
static int64_t sc_getsockopt(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
    { (void)f; return sys_getsockopt((int)a,(int)b,(int)c,(void*)d,(socklen_t*)e); }

/* Sparse dispatch table indexed by syscall number */
static syscall_fn_t g_syscall_table[256] = {
    [SYS_READ]      = sc_read,
    [SYS_WRITE]     = sc_write,
    [SYS_OPEN]      = sc_open,
    [SYS_CLOSE]     = sc_close,
    [SYS_STAT]      = sc_stat,
    [SYS_FSTAT]     = sc_fstat,
    [SYS_LSTAT]     = sc_lstat,
    [SYS_LSEEK]     = sc_lseek,
    [SYS_BRK]       = sc_brk,
    [SYS_DUP]       = sc_dup,
    [SYS_DUP2]      = sc_dup2,
    [SYS_EXIT]      = sc_exit,
    [SYS_GETCWD]    = sc_getcwd,
    [SYS_CHDIR]     = sc_chdir,
    [SYS_RENAME]    = sc_rename,
    [SYS_MKDIR]     = sc_mkdir,
    [SYS_RMDIR]     = sc_rmdir,
    [SYS_UNLINK]    = sc_unlink,
    [SYS_GETDENTS64]= sc_getdents64,
    [SYS_POLL]      = sc_poll,
    [SYS_IOCTL]     = sc_ioctl,
    [SYS_SOCKET]    = sc_socket,
    [SYS_CONNECT]   = sc_connect,
    [SYS_ACCEPT]    = sc_accept,
    [SYS_SENDTO]    = sc_sendto,
    [SYS_RECVFROM]  = sc_recvfrom,
    [SYS_SHUTDOWN]  = sc_shutdown,
    [SYS_BIND]      = sc_bind,
    [SYS_LISTEN]    = sc_listen,
    [SYS_GETSOCKNAME] = sc_getsockname,
    [SYS_GETPEERNAME]  = sc_getpeername,
    [SYS_SETSOCKOPT]   = sc_setsockopt,
    [SYS_GETSOCKOPT]   = sc_getsockopt,
};

/* ── INT 0x80 handler ─────────────────────────────────────────────────────── */
static void syscall_dispatch(cpu_regs_t *regs) {
    uint64_t nr = regs->rax;
    if (nr < 256 && g_syscall_table[nr]) {
        regs->rax = (uint64_t)g_syscall_table[nr](
            regs->rdi, regs->rsi, regs->rdx,
            regs->r10, regs->r8,  regs->r9);
    } else {
        KLOG_WARN("syscall: unknown nr=%llu\n", nr);
        regs->rax = (uint64_t)-ENOSYS;
    }
}

/* ── Init ────────────────────────────────────────────────────────────────── */
void syscall_init(void) {
    /* Register INT 0x80 with USER_GATE (DPL=3) so ring-3 can invoke it */
    idt_set_handler(0x80, isr_stub_table[0x80], IDT_USER_GATE, 0);
    idt_register_handler(0x80, syscall_dispatch);
    KLOG_INFO("syscall: INT 0x80 gate installed\n");
}
