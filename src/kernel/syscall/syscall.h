#pragma once
/* syscall/syscall.h — INT 0x80 + SYSCALL interface (Linux x86-64 ABI) */
#include "arch/x86_64/idt.h"
#include <stdint.h>

/* Linux x86-64 syscall numbers (subset implemented) */
#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_STAT        4
#define SYS_FSTAT       5
#define SYS_LSTAT       6
#define SYS_LSEEK       8
#define SYS_MMAP        9
#define SYS_MPROTECT    10
#define SYS_MUNMAP      11
#define SYS_BRK         12
#define SYS_RT_SIGACTION   13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN   15
#define SYS_PREAD64     17
#define SYS_PWRITE64    18
#define SYS_READV       19
#define SYS_GETTIMEOFDAY   96
#define SYS_WRITEV      20
#define SYS_ACCESS       21
#define SYS_PIPE         22
#define SYS_SCHED_YIELD  24
#define SYS_DUP         32
#define SYS_DUP2        33
#define SYS_NANOSLEEP   35
#define SYS_GETITIMER   36
#define SYS_ALARM       37
#define SYS_SETITIMER   38
#define SYS_GETPID      39
#define SYS_SENDFILE    40
#define SYS_CLONE       56
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_EXIT        60
#define SYS_WAIT4       61
#define SYS_KILL        62
#define SYS_UNAME       63
#define SYS_OPENAT      257
#define SYS_MKDIRAT     258
#define SYS_FCHOWNAT    260
#define SYS_RENAMEAT    264
#define SYS_FCNTL       72
#define SYS_GETCWD      79
#define SYS_CHDIR       80
#define SYS_RENAME      82
#define SYS_MKDIR       83
#define SYS_RMDIR       84
#define SYS_UNLINK      87
#define SYS_SYMLINK     88
#define SYS_READLINK    89
#define SYS_CHMOD       90
#define SYS_FCHMOD      91
#define SYS_CHOWN       92
#define SYS_FCHOWN      93
#define SYS_LCHOWN      94
#define SYS_UMASK       95
#define SYS_GETRLIMIT   97
#define SYS_TIMES       100
#define SYS_MOUNT      165
#define SYS_FUTIMESAT   261
#define SYS_GETUID      102
#define SYS_GETGID      104
#define SYS_SETUID      105
#define SYS_SETGID      106
#define SYS_GETEUID     107
#define SYS_GETEGID     108
#define SYS_SETREUID    113
#define SYS_SETREGID    114
#define SYS_SETPGID     109
#define SYS_GETPPID     110
#define SYS_GETGROUPS   115
#define SYS_SETGROUPS   116
#define SYS_SETRESUID   117
#define SYS_GETRESUID   118
#define SYS_SETRESGID   119
#define SYS_GETRESGID   120
#define SYS_SETSID      112
#define SYS_GETPGID     121
#define SYS_SETFSUID    122
#define SYS_SETFSGID    123
#define SYS_GETSID      124
#define SYS_CAPGET      125
#define SYS_CAPSET      126
#define SYS_PRCTL       157
#define SYS_ARCH_PRCTL  158
#define SYS_SYNC        162
#define SYS_GETTID      186
#define SYS_TKILL       200
#define SYS_FUTEX       202
#define SYS_GETDENTS64  217
#define SYS_SET_TID_ADDRESS 218
#define SYS_TGKILL      234
#define SYS_CLOCK_GETTIME   228
#define SYS_CLOCK_NANOSLEEP 230
#define SYS_EXIT_GROUP  231
#define SYS_FSTATAT     262
#define SYS_UNLINKAT    263
#define SYS_SYMLINKAT   266
#define SYS_READLINKAT  267
#define SYS_FCHMODAT    268
#define SYS_FACCESSAT   269
#define SYS_SET_ROBUST_LIST 273
#define SYS_GET_ROBUST_LIST 274
#define SYS_UTIMENSAT   280
#define SYS_UNSHARE     272
#define SYS_MADVISE     28
#define SYS_SENDMSG     46
#define SYS_RECVMSG     47
#define SYS_FLOCK       73
#define SYS_TRUNCATE    76
#define SYS_FTRUNCATE   77
#define SYS_LINK        86
#define SYS_MKNOD       133
#define SYS_STATFS      137
#define SYS_FSTATFS     138
#define SYS_EPOLL_CREATE   213
#define SYS_EPOLL_WAIT     232
#define SYS_EPOLL_CTL      233
#define SYS_EPOLL_PWAIT    281
#define SYS_FALLOCATE      285
#define SYS_TIMERFD_CREATE 283
#define SYS_TIMERFD_SETTIME 286
#define SYS_TIMERFD_GETTIME 287
#define SYS_ACCEPT4        288
#define SYS_SIGNALFD4      289
#define SYS_EVENTFD2       290
#define SYS_EPOLL_CREATE1  291
#define SYS_DUP3        292
#define SYS_PIPE2       293
#define SYS_INOTIFY_INIT1  294
#define SYS_PRLIMIT64   302
#define SYS_SETNS       308
#define SYS_SECCOMP     317
#define SYS_GETRANDOM   318
#define SYS_MEMFD_CREATE   319
#define SYS_CLONE3      435

/* *at constants */
#define AT_FDCWD        (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR    0x200
#define AT_EACCESS      0x200

/* ── Network syscall numbers (Linux x86-64 ABI) ─────────────────────────── */
#define SYS_POLL        7
#define SYS_IOCTL       16
#define SYS_SOCKET      41
#define SYS_CONNECT     42
#define SYS_ACCEPT      43
#define SYS_SENDTO      44
#define SYS_RECVFROM    45
#define SYS_SHUTDOWN    48
#define SYS_BIND        49
#define SYS_LISTEN      50
#define SYS_GETSOCKNAME 51
#define SYS_GETPEERNAME 52
#define SYS_SOCKETPAIR  53
#define SYS_SETSOCKOPT  54
#define SYS_GETSOCKOPT  55

/* arch_prctl commands */
#define ARCH_SET_GS     0x1001
#define ARCH_SET_FS     0x1002
#define ARCH_GET_FS     0x1003
#define ARCH_GET_GS     0x1004

/* prctl operations (subset) */
#define PR_GET_KEEPCAPS     7
#define PR_SET_KEEPCAPS     8
#define PR_SET_NAME         15
#define PR_GET_NAME         16
#define PR_GET_SECCOMP      21
#define PR_SET_SECCOMP      22
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39

/* seccomp */
#define SECCOMP_SET_MODE_STRICT 0
#define SECCOMP_SET_MODE_FILTER 1
#define SECCOMP_MODE_DISABLED   0
#define SECCOMP_MODE_STRICT     1
#define SECCOMP_MODE_FILTER     2

/* Linux open flags */
#define SYS_O_RDONLY    0
#define SYS_O_WRONLY    1
#define SYS_O_RDWR      2
#define SYS_O_CREAT     0100      /* octal 0100 = 0x40 */
#define SYS_O_TRUNC     01000     /* octal 01000 = 0x200 */
#define SYS_O_APPEND    02000     /* octal 02000 = 0x400 */
#define SYS_O_DIRECTORY 0200000

/* mmap flags */
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_ANON        MAP_ANONYMOUS
#define MAP_DENYWRITE   0x0800
#define MAP_FIXED_NOREPLACE 0x100000

/* mmap prot flags */
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

/* clone flags (subset) */
#define CLONE_VM        0x00000100
#define CLONE_FS        0x00000200
#define CLONE_FILES     0x00000400
#define CLONE_SIGHAND   0x00000800
#define CLONE_THREAD    0x00010000
#define CLONE_NEWNS     0x00020000
#define CLONE_SYSVSEM   0x00040000
#define CLONE_NEWUTS    0x04000000
#define CLONE_NEWIPC    0x08000000
#define CLONE_NEWUSER   0x10000000
#define CLONE_NEWPID    0x20000000
#define CLONE_NEWNET    0x40000000
#define CLONE_NEWCGROUP 0x02000000

/* futex ops */
#define FUTEX_WAIT      0
#define FUTEX_WAKE      1

/* Linux stat structure (x86-64 ABI) */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime;
    uint64_t st_atime_ns;
    uint64_t st_mtime;
    uint64_t st_mtime_ns;
    uint64_t st_ctime;
    uint64_t st_ctime_ns;
    int64_t  __unused[3];
} linux_stat_t;

/* Linux getdents64 entry */
typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];  /* null-terminated */
} __attribute__((packed)) linux_dirent64_t;

/* iovec for writev/readv */
typedef struct {
    void    *iov_base;
    uint64_t iov_len;
} iovec_t;

/* timespec for clock_gettime */
typedef struct {
    int64_t  tv_sec;
    int64_t  tv_nsec;
} kernel_timespec_t;

/* Linux statfs (x86-64) */
typedef struct {
    long     f_type;
    long     f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t  f_fsid[2];
    long     f_namelen;
    long     f_frsize;
    long     f_flags;
    long     f_spare[4];
} linux_statfs_t;

/* epoll_event (packed — Linux x86-64 ABI) */
#define EPOLLIN    0x00000001u
#define EPOLLPRI   0x00000002u
#define EPOLLOUT   0x00000004u
#define EPOLLERR   0x00000008u
#define EPOLLHUP   0x00000010u
#define EPOLLRDHUP 0x00002000u
#define EPOLLET    0x80000000u
#define EPOLLONESHOT 0x40000000u
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
typedef struct {
    uint32_t events;
    uint64_t data;
} __attribute__((packed)) epoll_event_t;

/* msghdr for sendmsg/recvmsg */
typedef struct {
    void       *msg_name;
    uint32_t    msg_namelen;
    iovec_t    *msg_iov;
    uint64_t    msg_iovlen;
    void       *msg_control;
    uint64_t    msg_controllen;
    int         msg_flags;
} msghdr_t;

void syscall_init(void);
void syscall_init_fast(void);  /* per-CPU SYSCALL MSR setup (call on each AP) */
