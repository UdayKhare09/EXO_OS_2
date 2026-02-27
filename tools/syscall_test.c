/* tools/syscall_test.c — Comprehensive syscall test for EXO_OS userspace
 *
 * Tests most implemented syscalls, printing PASS/FAIL for each.
 * Freestanding — uses raw SYSCALL instruction, no libc.
 *
 * Build: same flags as hello.c
 */

/* ── Raw syscall wrappers ─────────────────────────────────────────────── */

static long syscall0(long nr) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "a"(nr)
        : "rcx", "r11", "memory");
    return r;
}

static long syscall1(long nr, long a1) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "a"(nr), "D"(a1)
        : "rcx", "r11", "memory");
    return r;
}

static long syscall2(long nr, long a1, long a2) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "a"(nr), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory");
    return r;
}

static long syscall3(long nr, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory");
    return r;
}

static long syscall4(long nr, long a1, long a2, long a3, long a4) {
    long r;
    register long _a4 __asm__("r10") = a4;
    __asm__ volatile("syscall"
        : "=a"(r) : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(_a4)
        : "rcx", "r11", "memory");
    return r;
}

static long syscall6(long nr, long a1, long a2, long a3,
                     long a4, long a5, long a6) {
    long r;
    register long _a4 __asm__("r10") = a4;
    register long _a5 __asm__("r8")  = a5;
    register long _a6 __asm__("r9")  = a6;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(_a4), "r"(_a5), "r"(_a6)
        : "rcx", "r11", "memory");
    return r;
}

/* ── Linux x86-64 syscall numbers ────────────────────────────────────── */
#define SYS_READ          0
#define SYS_WRITE         1
#define SYS_OPEN          2
#define SYS_CLOSE         3
#define SYS_STAT          4
#define SYS_FSTAT         5
#define SYS_LSTAT         6
#define SYS_POLL          7
#define SYS_LSEEK         8
#define SYS_MMAP          9
#define SYS_MPROTECT      10
#define SYS_MUNMAP        11
#define SYS_BRK           12
#define SYS_PREAD64       17
#define SYS_PWRITE64      18
#define SYS_READV         19
#define SYS_GETTIMEOFDAY  96
#define SYS_TIMES         100
#define SYS_WRITEV        20
#define SYS_IOCTL         16
#define SYS_ACCESS        21
#define SYS_DUP           32
#define SYS_DUP2          33
#define SYS_SCHED_YIELD   24
#define SYS_NANOSLEEP     35
#define SYS_GETPID        39
#define SYS_GETPPID       110
#define SYS_GETUID        102
#define SYS_GETGID        104
#define SYS_GETEUID       107
#define SYS_GETEGID       108
#define SYS_GETTID        186
#define SYS_OPENAT        257
#define SYS_MKDIRAT       258
#define SYS_RENAMEAT      264
#define SYS_FCNTL         72
#define SYS_GETCWD        79
#define SYS_CHDIR         80
#define SYS_MKDIR         83
#define SYS_RMDIR         84
#define SYS_UNLINK        87
#define SYS_GETDENTS64    217
#define SYS_SET_TID_ADDRESS 218
#define SYS_FUTEX         202
#define SYS_CLOCK_GETTIME 228
#define SYS_CLOCK_NANOSLEEP 230
#define SYS_EXIT_GROUP    231
#define SYS_TGKILL        234
#define SYS_UMASK         95
#define SYS_GETRLIMIT     97
#define SYS_UNAME         63
#define SYS_DUP3          292
#define SYS_PRLIMIT64     302
#define SYS_GETRANDOM     318
#define SYS_FSTATAT       262
#define SYS_UNLINKAT      263
#define SYS_FACCESSAT     269
#define SYS_SET_ROBUST_LIST 273
#define SYS_GET_ROBUST_LIST 274
#define SYS_PIPE          22
#define SYS_RENAME        82
#define SYS_ARCH_PRCTL    158

/* errno values used by ABI conformance checks */
#define EBADF             9
#define EAGAIN            11
#define EINVAL            22
#define ETIMEDOUT         110

/* Open flags */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0100
#define O_TRUNC    01000
#define O_NONBLOCK 04000

#define AT_FDCWD   -100
#define AT_REMOVEDIR 0x200

#define FUTEX_WAIT         0
#define FUTEX_WAKE         1
#define FUTEX_WAIT_BITSET  9
#define FUTEX_WAKE_BITSET  10

/* poll/ioctl bits */
#define POLLIN     0x0001
#define POLLOUT    0x0004
#define POLLHUP    0x0010

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TIOCGWINSZ 0x5413
#define FIONBIO    0x5421

/* mmap flags */
#define PROT_READ    1
#define PROT_WRITE   2
#define PROT_EXEC    4
#define MAP_PRIVATE  2
#define MAP_ANONYMOUS 32

/* arch_prctl codes */
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

/* ── Minimal types ───────────────────────────────────────────────────── */
typedef unsigned long  size_t;
typedef long           ssize_t;
typedef long long      int64_t;
typedef unsigned char  uint8_t;
typedef unsigned int   uint32_t;
typedef unsigned long  uint64_t;

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

struct timespec {
    long tv_sec;
    long tv_nsec;
};

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct tms {
    long tms_utime;
    long tms_stime;
    long tms_cutime;
    long tms_cstime;
};

#define TIMER_ABSTIME 1

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct rlimit {
    unsigned long rlim_cur;
    unsigned long rlim_max;
};

#define RLIMIT_NOFILE 7

typedef unsigned int   tcflag_t;
typedef unsigned char  cc_t;
typedef unsigned int   speed_t;

#define NCCS 19

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

struct pollfd {
    int   fd;
    short events;
    short revents;
};

/* Minimal linux_stat (128 bytes, matches kernel struct) */
struct stat {
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
    uint64_t st_atime_nsec;
    uint64_t st_mtime;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime;
    uint64_t st_ctime_nsec;
    long     __unused[3];
};

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char     d_name[1];
};

/* ── String helpers (no libc) ────────────────────────────────────────── */

static size_t my_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void my_memset(void *p, int v, size_t n) {
    unsigned char *b = p;
    while (n--) *b++ = (unsigned char)v;
}

/* ── Output helpers ──────────────────────────────────────────────────── */

static void print(const char *s) {
    syscall3(SYS_WRITE, 1, (long)s, (long)my_strlen(s));
}

static void println(const char *s) {
    print(s);
    print("\n");
}

/* Print a signed decimal number */
static void print_long(long v) {
    char buf[24];
    int i = 23;
    buf[i] = '\0';
    int neg = (v < 0);
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) { buf[--i] = '0'; }
    else { while (u) { buf[--i] = '0' + (u % 10); u /= 10; } }
    if (neg) buf[--i] = '-';
    print(buf + i);
}

/* Print a hex number with 0x prefix */
static void print_hex(unsigned long v) {
    char buf[20];
    int i = 19;
    buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    else { while (v) { int d = v & 0xF; buf[--i] = d < 10 ? '0'+d : 'a'+d-10; v >>= 4; } }
    buf[--i] = 'x';
    buf[--i] = '0';
    print(buf + i);
}

/* ── Test infrastructure ─────────────────────────────────────────────── */

static int tests_run = 0, tests_passed = 0;

static void test_result(const char *name, int pass) {
    tests_run++;
    if (pass) {
        tests_passed++;
        print("  \033[1;32mPASS\033[0m ");
    } else {
        print("  \033[1;31mFAIL\033[0m ");
    }
    println(name);
}

/* ═══════════════════════════════════════════════════════════════════════ *
 *  Tests                                                                 *
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── write ────────────────────────────────────────────────────────────── */
static void test_write(void) {
    static const char msg[] = "  [write test output]\n";
    long r = syscall3(SYS_WRITE, 1, (long)msg, (long)(sizeof(msg)-1));
    test_result("write(1, msg, n) returns n", r == (long)(sizeof(msg)-1));
}

/* ── read from /dev/zero ──────────────────────────────────────────────── */
static void test_read_zero(void) {
    int fd = (int)syscall3(SYS_OPEN, (long)"/dev/zero", O_RDONLY, 0);
    int ok = (fd >= 0);
    if (ok) {
        char buf[8];
        long r = syscall3(SYS_READ, fd, (long)buf, 8);
        ok = (r == 8) && (buf[0] == 0) && (buf[7] == 0);
        syscall1(SYS_CLOSE, fd);
    }
    test_result("read(/dev/zero) returns zeros", ok);
}

/* ── open / close ────────────────────────────────────────────────────── */
static void test_open_close(void) {
    int fd = (int)syscall3(SYS_OPEN, (long)"/dev/null", O_RDONLY, 0);
    test_result("open(/dev/null) returns fd >= 0", fd >= 0);
    if (fd >= 0) {
        long r = syscall1(SYS_CLOSE, fd);
        test_result("close(fd) returns 0", r == 0);
    }
}

/* ── stat ────────────────────────────────────────────────────────────── */
static void test_stat(void) {
    struct stat st;
    my_memset(&st, 0, sizeof(st));
    long r = syscall2(SYS_STAT, (long)"/", (long)&st);
    test_result("stat(\"/\") returns 0", r == 0);
    test_result("stat(\"/\") mode is directory", (st.st_mode & 0170000) == 0040000);
}

/* ── fstat ───────────────────────────────────────────────────────────── */
static void test_fstat(void) {
    int fd = (int)syscall3(SYS_OPEN, (long)"/dev/tty", O_RDWR, 0);
    int ok = (fd >= 0);
    if (ok) {
        struct stat st;
        my_memset(&st, 0, sizeof(st));
        long r = syscall2(SYS_FSTAT, fd, (long)&st);
        ok = (r == 0);
        syscall1(SYS_CLOSE, fd);
    }
    test_result("fstat(tty_fd) returns 0", ok);
}

/* ── lstat ───────────────────────────────────────────────────────────── */
static void test_lstat(void) {
    struct stat st;
    my_memset(&st, 0, sizeof(st));
    long r = syscall2(SYS_LSTAT, (long)"/dev/null", (long)&st);
    test_result("lstat(\"/dev/null\") returns 0", r == 0);
}

/* ── lseek ───────────────────────────────────────────────────────────── */
static void test_lseek(void) {
    int fd = (int)syscall3(SYS_OPEN, (long)"/dev/zero", O_RDONLY, 0);
    int ok = (fd >= 0);
    if (ok) {
        long r = syscall3(SYS_LSEEK, fd, 100, 0 /*SEEK_SET*/);
        ok = (r == 100);
        syscall1(SYS_CLOSE, fd);
    }
    test_result("lseek(fd, 100, SEEK_SET) == 100", ok);
}

/* ── mmap / munmap ───────────────────────────────────────────────────── */
static void test_mmap_munmap(void) {
    long addr = syscall6(SYS_MMAP, 0, 4096,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1, 0);
    int ok = (addr > 0 && addr != -1);
    test_result("mmap(anon, 4096, RW) returns valid address", ok);
    if (ok) {
        /* Write and read back */
        volatile char *p = (volatile char *)addr;
        p[0] = 0x42;
        p[4095] = 0x55;
        int rw_ok = (p[0] == 0x42 && p[4095] == 0x55);
        test_result("mmap page is readable/writable", rw_ok);

        long r = syscall2(SYS_MUNMAP, addr, 4096);
        test_result("munmap returns 0", r == 0);
    }
}

/* ── file-backed mmap (MAP_PRIVATE) ─────────────────────────────────── */
static void test_mmap_file_private(void) {
    static const char path[] = "/tmp/sctest_mmap_file.bin";
    static const char data[] = "mmap_file_payload";

    int fd = (int)syscall3(SYS_OPEN, (long)path,
                           O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        test_result("mmap file: open/create", 0);
        return;
    }

    long w = syscall3(SYS_WRITE, fd, (long)data, (long)(sizeof(data) - 1));
    test_result("mmap file: write seed data", w == (long)(sizeof(data) - 1));
    if (w < 0) {
        syscall1(SYS_CLOSE, fd);
        return;
    }

    long addr = syscall6(SYS_MMAP, 0, 4096,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE,
                         fd, 0);
    int ok = (addr > 0 && addr != -1);
    test_result("mmap file: MAP_PRIVATE mapping succeeds", ok);

    if (ok) {
        volatile char *p = (volatile char *)addr;
        int same = 1;
        for (size_t i = 0; i < sizeof(data) - 1; i++) {
            if (p[i] != data[i]) {
                same = 0;
                break;
            }
        }
        test_result("mmap file: mapped bytes match file", same);

        p[0] = 'X';

        long s = syscall3(SYS_LSEEK, fd, 0, 0);
        char c = 0;
        long r = (s >= 0) ? syscall3(SYS_READ, fd, (long)&c, 1) : -1;
        test_result("mmap file: private write doesn't modify file", (r == 1) && (c == data[0]));

        syscall2(SYS_MUNMAP, addr, 4096);
    }

    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_UNLINK, (long)path);
}

/* ── mprotect ────────────────────────────────────────────────────────── */
static void test_mprotect(void) {
    long addr = syscall6(SYS_MMAP, 0, 4096,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1, 0);
    int ok = (addr > 0 && addr != -1);
    if (ok) {
        long r = syscall3(SYS_MPROTECT, addr, 4096, PROT_READ);
        ok = (r == 0);
        syscall2(SYS_MUNMAP, addr, 4096);
    }
    test_result("mprotect(page, PROT_READ) returns 0", ok);
}

/* ── brk ─────────────────────────────────────────────────────────────── */
static void test_brk(void) {
    long cur = syscall1(SYS_BRK, 0);
    int ok = (cur > 0);
    test_result("brk(0) returns current brk > 0", ok);
    if (ok) {
        /* Expand by one page */
        long new_brk = syscall1(SYS_BRK, cur + 4096);
        test_result("brk(brk+4096) expands heap", new_brk >= cur + 4096);
        /* Shrink back */
        syscall1(SYS_BRK, cur);
    }
}

/* ── writev ──────────────────────────────────────────────────────────── */
static void test_writev(void) {
    static const char p1[] = "  [writev";
    static const char p2[] = " test]\n";
    struct iovec iov[2] = {
        { (void *)p1, sizeof(p1)-1 },
        { (void *)p2, sizeof(p2)-1 },
    };
    long r = syscall3(SYS_WRITEV, 1, (long)iov, 2);
    long expected = (long)(sizeof(p1)-1 + sizeof(p2)-1);
    test_result("writev(stdout, iov, 2) returns total bytes", r == expected);
}

static void test_readv(void) {
    int p[2] = {-1, -1};
    long r = syscall1(SYS_PIPE, (long)p);
    if (r != 0) {
        test_result("readv: pipe setup", 0);
        return;
    }

    static const char msg[] = "abcdef";
    r = syscall3(SYS_WRITE, p[1], (long)msg, 6);
    if (r != 6) {
        test_result("readv: seed pipe data", 0);
        syscall1(SYS_CLOSE, p[0]);
        syscall1(SYS_CLOSE, p[1]);
        return;
    }

    char a[3], b[3];
    struct iovec iov[2] = {
        { a, 3 },
        { b, 3 },
    };

    r = syscall3(SYS_READV, p[0], (long)iov, 2);
    test_result("readv(pipe, iov, 2) returns 6", r == 6);
    int ok = (a[0]=='a' && a[1]=='b' && a[2]=='c' &&
              b[0]=='d' && b[1]=='e' && b[2]=='f');
    test_result("readv splits data across iovecs", ok);

    syscall1(SYS_CLOSE, p[0]);
    syscall1(SYS_CLOSE, p[1]);
}

static void test_pread_pwrite(void) {
    static const char path[] = "/tmp/sctest_pread_pwrite";
    int fd = (int)syscall3(SYS_OPEN, (long)path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        test_result("pread/pwrite: open temp file", 0);
        return;
    }

    static const char seed[] = "abcdef";
    long r = syscall3(SYS_WRITE, fd, (long)seed, 6);
    test_result("pread/pwrite: seed write", r == 6);

    char pbuf[3] = {0,0,0};
    r = syscall4(SYS_PREAD64, fd, (long)pbuf, 3, 2);
    test_result("pread64(fd,3,off=2) returns 3", r == 3);
    test_result("pread64 reads expected bytes", pbuf[0]=='c' && pbuf[1]=='d' && pbuf[2]=='e');

    r = syscall3(SYS_LSEEK, fd, 0, 0);
    char first = 0;
    long rr = syscall3(SYS_READ, fd, (long)&first, 1);
    test_result("pread64 does not advance file offset", rr == 1 && first == 'a');

    static const char patch[] = "ZZ";
    r = syscall4(SYS_PWRITE64, fd, (long)patch, 2, 1);
    test_result("pwrite64(fd,2,off=1) returns 2", r == 2);

    syscall3(SYS_LSEEK, fd, 0, 0);
    char all[6] = {0};
    rr = syscall3(SYS_READ, fd, (long)all, 6);
    test_result("pwrite64 writes at offset", rr == 6 &&
                all[0]=='a' && all[1]=='Z' && all[2]=='Z' &&
                all[3]=='d' && all[4]=='e' && all[5]=='f');

    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_UNLINK, (long)path);
}

/* ── access ──────────────────────────────────────────────────────────── */
static void test_access(void) {
    long r = syscall2(SYS_ACCESS, (long)"/dev/null", 0 /*F_OK*/);
    test_result("access(\"/dev/null\", F_OK) == 0", r == 0);
    r = syscall2(SYS_ACCESS, (long)"/nonexistent_path_xyz", 0);
    test_result("access(\"/nonexistent\", F_OK) fails", r < 0);
}

/* ── dup / dup2 ──────────────────────────────────────────────────────── */
static void test_dup_dup2(void) {
    int fd2 = (int)syscall1(SYS_DUP, 1);
    test_result("dup(stdout) returns new fd >= 3", fd2 >= 3);
    if (fd2 >= 0) {
        /* Write via the dup'd fd */
        static const char msg[] = "  [dup fd write]\n";
        long r = syscall3(SYS_WRITE, fd2, (long)msg, (long)(sizeof(msg)-1));
        test_result("write via dup'd fd works", r == (long)(sizeof(msg)-1));
        syscall1(SYS_CLOSE, fd2);
    }

    int fd3 = (int)syscall2(SYS_DUP2, 1, 10);
    test_result("dup2(stdout, 10) returns 10", fd3 == 10);
    if (fd3 == 10) syscall1(SYS_CLOSE, 10);
}

/* ── dup3 ────────────────────────────────────────────────────────────── */
static void test_dup3(void) {
    int fd = (int)syscall3(SYS_DUP3, 1, 11, 0);
    test_result("dup3(stdout, 11, 0) returns 11", fd == 11);
    if (fd == 11) syscall1(SYS_CLOSE, 11);
}

/* ── getpid / getppid ────────────────────────────────────────────────── */
static void test_pid(void) {
    long pid = syscall0(SYS_GETPID);
    test_result("getpid() returns pid > 0", pid > 0);
    print("    pid="); print_long(pid); print("\n");

    long ppid = syscall0(SYS_GETPPID);
    test_result("getppid() returns ppid >= 0", ppid >= 0);
    print("    ppid="); print_long(ppid); print("\n");
}

/* ── getuid / getgid / geteuid / getegid ──────────────────────────────── */
static void test_ids(void) {
    long uid  = syscall0(SYS_GETUID);
    long gid  = syscall0(SYS_GETGID);
    long euid = syscall0(SYS_GETEUID);
    long egid = syscall0(SYS_GETEGID);
    test_result("getuid() returns 0 (root)", uid == 0);
    test_result("getgid() returns 0", gid == 0);
    test_result("geteuid() == 0", euid == 0);
    test_result("getegid() == 0", egid == 0);
}

static void test_tid_and_tgkill(void) {
    long pid = syscall0(SYS_GETPID);
    long tid = syscall0(SYS_GETTID);
    test_result("gettid() > 0", tid > 0);
    test_result("single-thread pid == tid", pid == tid);

    long r = syscall3(SYS_TGKILL, pid, tid, 0);
    test_result("tgkill(pid, tid, 0) returns 0", r == 0);
}

static void test_robust_list(void) {
    unsigned long robust_head[3] = {0, 0, 0};
    unsigned long out_head = 0;
    unsigned long out_len = 0;

    long r = syscall2(SYS_SET_ROBUST_LIST, (long)robust_head, 24);
    test_result("set_robust_list(head,24) returns 0", r == 0);

    r = syscall3(SYS_GET_ROBUST_LIST, 0, (long)&out_head, (long)&out_len);
    test_result("get_robust_list(0,...) returns 0", r == 0);
    test_result("get_robust_list head matches", out_head == (unsigned long)robust_head);
    test_result("get_robust_list len is 24", out_len == 24);
}

static void test_futex_phase2(void) {
    volatile uint32_t fut = 1;
    struct timespec ts;

    long r = syscall6(SYS_FUTEX, (long)&fut, FUTEX_WAIT, 0, 0, 0, 0);
    test_result("futex WAIT mismatch returns -EAGAIN", r == -EAGAIN);

    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    r = syscall6(SYS_FUTEX, (long)&fut, FUTEX_WAIT_BITSET, 1,
                 (long)&ts, 0, (long)-1);
    test_result("futex WAIT_BITSET zero-timeout returns -ETIMEDOUT", r == -ETIMEDOUT);

    r = syscall6(SYS_FUTEX, (long)&fut, FUTEX_WAKE_BITSET, 1, 0, 0, 0);
    test_result("futex WAKE_BITSET zero mask returns -EINVAL", r == -EINVAL);
}

/* ── getcwd ──────────────────────────────────────────────────────────── */
static void test_getcwd(void) {
    char buf[256];
    my_memset(buf, 0, sizeof(buf));
    long r = syscall2(SYS_GETCWD, (long)buf, sizeof(buf));
    int ok = (r > 0 && buf[0] == '/');
    test_result("getcwd() returns '/' path", ok);
    if (ok) { print("    cwd="); println(buf); }
}

/* ── chdir / mkdir / rmdir ───────────────────────────────────────────── */
static void test_dir_ops(void) {
    /* Make a temp dir under /tmp */
    long r = syscall2(SYS_MKDIR, (long)"/tmp/sctest_dir", 0755);
    test_result("mkdir(\"/tmp/sctest_dir\") succeeds", r == 0);

    /* chdir into it */
    r = syscall1(SYS_CHDIR, (long)"/tmp/sctest_dir");
    test_result("chdir(\"/tmp/sctest_dir\") succeeds", r == 0);

    /* getcwd should reflect the new path */
    char buf[256];
    my_memset(buf, 0, sizeof(buf));
    syscall2(SYS_GETCWD, (long)buf, sizeof(buf));
    int ok = (my_strcmp(buf, "/tmp/sctest_dir") == 0);
    test_result("getcwd() reflects chdir", ok);

    /* Go back to root */
    syscall1(SYS_CHDIR, (long)"/");

    /* rmdir */
    r = syscall1(SYS_RMDIR, (long)"/tmp/sctest_dir");
    test_result("rmdir(\"/tmp/sctest_dir\") succeeds", r == 0);
}

/* ── open (O_CREAT) / write / read / unlink ─────────────────────────── */
static void test_file_rw(void) {
    static const char path[] = "/tmp/sctest_file.txt";
    static const char data[] = "exo syscall test data\n";

    /* Create file */
    int fd = (int)syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    test_result("open(O_CREAT|O_WRONLY) creates file", fd >= 0);
    if (fd < 0) return;

    /* Write data */
    long wn = syscall3(SYS_WRITE, fd, (long)data, (long)(sizeof(data)-1));
    test_result("write to new file returns n", wn == (long)(sizeof(data)-1));
    syscall1(SYS_CLOSE, fd);

    /* Open for read */
    fd = (int)syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    test_result("re-open for read succeeds", fd >= 0);
    if (fd >= 0) {
        char rbuf[64];
        my_memset(rbuf, 0, sizeof(rbuf));
        long rn = syscall3(SYS_READ, fd, (long)rbuf, sizeof(rbuf)-1);
        test_result("read back correct length", rn == wn);
        syscall1(SYS_CLOSE, fd);
    }

    /* stat the file */
    struct stat st;
    my_memset(&st, 0, sizeof(st));
    long sr = syscall2(SYS_STAT, (long)path, (long)&st);
    test_result("stat(file) returns 0", sr == 0);
    test_result("stat shows correct size", st.st_size == (long)(sizeof(data)-1));

    /* unlink */
    long ur = syscall1(SYS_UNLINK, (long)path);
    test_result("unlink(file) returns 0", ur == 0);

    /* Confirm file is gone */
    my_memset(&st, 0, sizeof(st));
    sr = syscall2(SYS_STAT, (long)path, (long)&st);
    test_result("stat(deleted file) fails", sr < 0);
}

/* ── rename ──────────────────────────────────────────────────────────── */
static void test_rename(void) {
    static const char src[] = "/tmp/sctest_rename_src";
    static const char dst[] = "/tmp/sctest_rename_dst";

    /* Create source file */
    int fd = (int)syscall3(SYS_OPEN, (long)src, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { test_result("rename: create src", 0); return; }
    syscall3(SYS_WRITE, fd, (long)"data", 4);
    syscall1(SYS_CLOSE, fd);

    long r = syscall2(SYS_RENAME, (long)src, (long)dst);
    test_result("rename(src, dst) returns 0", r == 0);

    struct stat st;
    my_memset(&st, 0, sizeof(st));
    test_result("renamed file accessible at dst", syscall2(SYS_STAT, (long)dst, (long)&st) == 0);
    test_result("src no longer exists", syscall2(SYS_STAT, (long)src, (long)&st) < 0);

    syscall1(SYS_UNLINK, (long)dst);
}

/* ── fcntl ───────────────────────────────────────────────────────────── */
static void test_fcntl(void) {
    int fd = (int)syscall3(SYS_OPEN, (long)"/dev/null", O_RDONLY, 0);
    int ok = (fd >= 0);
    if (ok) {
        long r = syscall3(SYS_FCNTL, fd, 3 /*F_GETFL*/, 0);
        test_result("fcntl(F_GETFL) returns flags >= 0", r >= 0);
        r = syscall3(SYS_FCNTL, fd, 1 /*F_GETFD*/, 0);
        test_result("fcntl(F_GETFD) returns fd_flags >= 0", r >= 0);
        syscall1(SYS_CLOSE, fd);
    } else {
        test_result("fcntl test: open /dev/null", 0);
    }
}

/* ── umask ───────────────────────────────────────────────────────────── */
static void test_umask(void) {
    long r = syscall1(SYS_UMASK, 022);
    /* Kernel always returns 022 for now */
    test_result("umask(022) returns a mask value", r >= 0);
}

/* ── clock_gettime ───────────────────────────────────────────────────── */
static void test_clock_gettime(void) {
    struct timespec ts;
    my_memset(&ts, 0, sizeof(ts));
    long r = syscall2(SYS_CLOCK_GETTIME, 1 /*CLOCK_MONOTONIC*/, (long)&ts);
    test_result("clock_gettime(MONOTONIC) returns 0", r == 0);
    test_result("clock_gettime gives non-negative seconds", ts.tv_sec >= 0);
    print("    time="); print_long(ts.tv_sec);
    print("."); print_long(ts.tv_nsec / 1000000); println("s");
}

static long timespec_to_ms(const struct timespec *ts) {
    return ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
}

/* ── sched_yield ─────────────────────────────────────────────────────── */
static void test_sched_yield(void) {
    long r = syscall0(SYS_SCHED_YIELD);
    test_result("sched_yield() returns 0", r == 0);
}

/* ── gettimeofday ────────────────────────────────────────────────────── */
static void test_gettimeofday(void) {
    struct timeval tv;
    my_memset(&tv, 0, sizeof(tv));
    long r = syscall2(SYS_GETTIMEOFDAY, (long)&tv, 0);
    test_result("gettimeofday() returns 0", r == 0);
    test_result("gettimeofday: tv_usec in range", tv.tv_usec >= 0 && tv.tv_usec < 1000000);
}

/* ── nanosleep ───────────────────────────────────────────────────────── */
static void test_nanosleep(void) {
    struct timespec before, after, req;
    my_memset(&before, 0, sizeof(before));
    my_memset(&after, 0, sizeof(after));

    long r = syscall2(SYS_CLOCK_GETTIME, 1 /*CLOCK_MONOTONIC*/, (long)&before);
    if (r != 0) {
        test_result("nanosleep pre clock_gettime", 0);
        return;
    }

    req.tv_sec = 0;
    req.tv_nsec = 10 * 1000 * 1000;
    r = syscall2(SYS_NANOSLEEP, (long)&req, 0);
    test_result("nanosleep(10ms) returns 0", r == 0);

    r = syscall2(SYS_CLOCK_GETTIME, 1 /*CLOCK_MONOTONIC*/, (long)&after);
    if (r != 0) {
        test_result("nanosleep post clock_gettime", 0);
        return;
    }

    long delta = timespec_to_ms(&after) - timespec_to_ms(&before);
    test_result("nanosleep advances monotonic clock", delta >= 1);
}

static void test_clock_nanosleep(void) {
    struct timespec before, after, req;
    my_memset(&before, 0, sizeof(before));
    my_memset(&after, 0, sizeof(after));

    long r = syscall2(SYS_CLOCK_GETTIME, 1 /*CLOCK_MONOTONIC*/, (long)&before);
    if (r != 0) {
        test_result("clock_nanosleep pre clock_gettime", 0);
        return;
    }

    req.tv_sec = 0;
    req.tv_nsec = 5 * 1000 * 1000;
    r = syscall4(SYS_CLOCK_NANOSLEEP, 1 /*CLOCK_MONOTONIC*/, 0, (long)&req, 0);
    test_result("clock_nanosleep(relative) returns 0", r == 0);

    r = syscall2(SYS_CLOCK_GETTIME, 1 /*CLOCK_MONOTONIC*/, (long)&after);
    if (r != 0) {
        test_result("clock_nanosleep post clock_gettime", 0);
        return;
    }

    long delta = timespec_to_ms(&after) - timespec_to_ms(&before);
    test_result("clock_nanosleep advances monotonic clock", delta >= 1);

    req.tv_sec = after.tv_sec;
    req.tv_nsec = after.tv_nsec + 5 * 1000 * 1000;
    if (req.tv_nsec >= 1000000000L) {
        req.tv_sec += 1;
        req.tv_nsec -= 1000000000L;
    }
    r = syscall4(SYS_CLOCK_NANOSLEEP, 1 /*CLOCK_MONOTONIC*/, TIMER_ABSTIME, (long)&req, 0);
    test_result("clock_nanosleep(TIMER_ABSTIME) returns 0", r == 0);
}

static void test_times(void) {
    struct tms tm;
    my_memset(&tm, 0, sizeof(tm));
    long r = syscall1(SYS_TIMES, (long)&tm);
    test_result("times() returns non-negative clock ticks", r >= 0);
    test_result("times(): user time non-negative", tm.tms_utime >= 0);
}

/* ── getdents64 ──────────────────────────────────────────────────────── */
static void test_getdents64(void) {
    int fd = (int)syscall3(SYS_OPEN, (long)"/dev", O_RDONLY, 0);
    int ok = (fd >= 0);
    if (ok) {
        char buf[1024];
        my_memset(buf, 0, sizeof(buf));
        long r = syscall3(SYS_GETDENTS64, fd, (long)buf, sizeof(buf));
        ok = (r > 0);
        test_result("getdents64(\"/dev\") returns > 0 bytes", ok);
        if (ok) {
            /* Count entries */
            int count = 0;
            long pos = 0;
            while (pos < r) {
                struct linux_dirent64 *de = (struct linux_dirent64 *)(buf + pos);
                if (de->d_reclen == 0) break;
                count++;
                pos += de->d_reclen;
            }
            test_result("getdents64 has at least 1 entry", count >= 1);
            print("    /dev entries: "); print_long(count); print("\n");
        }
        syscall1(SYS_CLOSE, fd);
    } else {
        test_result("getdents64: open /dev", 0);
    }
}

/* ── set_tid_address ─────────────────────────────────────────────────── */
static void test_set_tid_address(void) {
    int tid_loc = 0;
    long r = syscall1(SYS_SET_TID_ADDRESS, (long)&tid_loc);
    test_result("set_tid_address() returns tid > 0", r > 0);
}

/* ── fstatat ─────────────────────────────────────────────────────────── */
static void test_fstatat(void) {
    struct stat st;
    my_memset(&st, 0, sizeof(st));
    /* AT_FDCWD = -100 */
    long r = syscall4(SYS_FSTATAT, -100, (long)"/dev/null", (long)&st, 0);
    test_result("fstatat(AT_FDCWD, \"/dev/null\") returns 0", r == 0);
}

/* ── openat/mkdirat/unlinkat ────────────────────────────────────────── */
static void test_at_family(void) {
    long r = syscall3(SYS_MKDIRAT, AT_FDCWD, (long)"/tmp/at_test", 0755);
    int ok_mkdir = (r == 0 || r == -17); /* allow EEXIST */
    test_result("mkdirat(AT_FDCWD, /tmp/at_test)", ok_mkdir);

    int dfd = (int)syscall3(SYS_OPEN, (long)"/tmp/at_test", O_RDONLY, 0);
    test_result("open(/tmp/at_test) as dirfd", dfd >= 0);
    if (dfd < 0) return;

    int fd = (int)syscall4(SYS_OPENAT, dfd, (long)"f.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    test_result("openat(dirfd, f.txt, O_CREAT)", fd >= 0);
    if (fd >= 0) syscall1(SYS_CLOSE, fd);

    r = syscall4(SYS_FACCESSAT, dfd, (long)"f.txt", 0, 0);
    test_result("faccessat(dirfd, f.txt, F_OK)", r == 0);

    r = syscall4(SYS_RENAMEAT, dfd, (long)"f.txt", dfd, (long)"g.txt");
    test_result("renameat(dirfd, f.txt -> g.txt)", r == 0);

    r = syscall4(SYS_FACCESSAT, dfd, (long)"g.txt", 0, 0);
    test_result("faccessat(dirfd, g.txt, F_OK)", r == 0);

    r = syscall3(SYS_UNLINKAT, dfd, (long)"g.txt", 0);
    test_result("unlinkat(dirfd, g.txt)", r == 0);

    syscall1(SYS_CLOSE, dfd);
    r = syscall3(SYS_UNLINKAT, AT_FDCWD, (long)"/tmp/at_test", AT_REMOVEDIR);
    test_result("unlinkat(AT_FDCWD, /tmp/at_test, AT_REMOVEDIR)", r == 0);
}

/* ── pipe ────────────────────────────────────────────────────────────── */
static void test_pipe(void) {
    int pipefd[2] = {-1, -1};
    long r = syscall1(SYS_PIPE, (long)pipefd);
    test_result("pipe() returns 0", r == 0);
    if (r == 0) {
        test_result("pipe: read fd >= 0", pipefd[0] >= 0);
        test_result("pipe: write fd >= 0", pipefd[1] >= 0);
        /* Write then read */
        static const char pipe_msg[] = "pipe_test";
        long wn = syscall3(SYS_WRITE, pipefd[1], (long)pipe_msg, 9);
        test_result("pipe: write 9 bytes", wn == 9);
        char rbuf[16];
        my_memset(rbuf, 0, sizeof(rbuf));
        long rn = syscall3(SYS_READ, pipefd[0], (long)rbuf, sizeof(rbuf));
        test_result("pipe: read back 9 bytes", rn == 9);
        syscall1(SYS_CLOSE, pipefd[0]);
        syscall1(SYS_CLOSE, pipefd[1]);
    }
}

/* ── poll ────────────────────────────────────────────────────────────── */
static void test_poll_phase3(void) {
    int pipefd[2] = {-1, -1};
    long r = syscall1(SYS_PIPE, (long)pipefd);
    if (r != 0) {
        test_result("poll: pipe setup", 0);
        return;
    }

    struct pollfd pfd;
    pfd.fd = pipefd[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    r = syscall3(SYS_POLL, (long)&pfd, 1, 0);
    test_result("poll(empty pipe, timeout=0) returns 0", r == 0);

    char ch = 'x';
    r = syscall3(SYS_WRITE, pipefd[1], (long)&ch, 1);
    test_result("poll: write 1 byte into pipe", r == 1);

    pfd.revents = 0;
    r = syscall3(SYS_POLL, (long)&pfd, 1, 0);
    test_result("poll(pipe readable) returns 1", r == 1);
    test_result("poll(pipe readable) has POLLIN", (pfd.revents & POLLIN) != 0);

    char out;
    syscall3(SYS_READ, pipefd[0], (long)&out, 1);
    syscall1(SYS_CLOSE, pipefd[1]);

    pfd.revents = 0;
    r = syscall3(SYS_POLL, (long)&pfd, 1, 0);
    test_result("poll(pipe read end after writer close) has POLLHUP", (r == 1) && ((pfd.revents & POLLHUP) != 0));

    syscall1(SYS_CLOSE, pipefd[0]);

    int tty = (int)syscall3(SYS_OPEN, (long)"/dev/tty", O_RDWR, 0);
    if (tty >= 0) {
        struct pollfd tp;
        tp.fd = tty;
        tp.events = POLLOUT;
        tp.revents = 0;
        r = syscall3(SYS_POLL, (long)&tp, 1, 0);
        test_result("poll(/dev/tty, POLLOUT, 0) returns writable", (r == 1) && ((tp.revents & POLLOUT) != 0));
        syscall1(SYS_CLOSE, tty);
    } else {
        test_result("poll tty test: open /dev/tty", 0);
    }
}

/* ── ioctl (tty + FIONBIO) ──────────────────────────────────────────── */
static void test_ioctl_phase3(void) {
    int tty = (int)syscall3(SYS_OPEN, (long)"/dev/tty", O_RDWR, 0);
    if (tty < 0) {
        test_result("ioctl: open /dev/tty", 0);
        return;
    }

    struct termios tio;
    my_memset(&tio, 0, sizeof(tio));
    long r = syscall3(SYS_IOCTL, tty, TCGETS, (long)&tio);
    test_result("ioctl(TCGETS) on /dev/tty returns 0", r == 0);

    tcflag_t old_lflag = tio.c_lflag;
    tio.c_lflag ^= 0x2;
    r = syscall3(SYS_IOCTL, tty, TCSETS, (long)&tio);
    test_result("ioctl(TCSETS) on /dev/tty returns 0", r == 0);

    struct termios tio2;
    my_memset(&tio2, 0, sizeof(tio2));
    r = syscall3(SYS_IOCTL, tty, TCGETS, (long)&tio2);
    test_result("ioctl(TCGETS) reflects updated c_lflag", (r == 0) && (tio2.c_lflag == (old_lflag ^ 0x2)));

    struct winsize ws;
    my_memset(&ws, 0, sizeof(ws));
    r = syscall3(SYS_IOCTL, tty, TIOCGWINSZ, (long)&ws);
    test_result("ioctl(TIOCGWINSZ) returns 0", r == 0);
    test_result("winsize rows/cols are non-zero", ws.ws_row > 0 && ws.ws_col > 0);
    syscall1(SYS_CLOSE, tty);

    int pipefd[2] = {-1, -1};
    r = syscall1(SYS_PIPE, (long)pipefd);
    if (r != 0) {
        test_result("ioctl(FIONBIO): pipe setup", 0);
        return;
    }

    int nb = 1;
    r = syscall3(SYS_IOCTL, pipefd[0], FIONBIO, (long)&nb);
    test_result("ioctl(FIONBIO) on pipe read end returns 0", r == 0);

    char c = 0;
    r = syscall3(SYS_READ, pipefd[0], (long)&c, 1);
    test_result("nonblocking pipe read on empty pipe returns -EAGAIN", r == -EAGAIN);

    syscall1(SYS_CLOSE, pipefd[0]);
    syscall1(SYS_CLOSE, pipefd[1]);
}

/* ── arch_prctl (FS base / TLS) ──────────────────────────────────────── */
static void test_arch_prctl(void) {
    long dummy = 0xDEADBEEF;
    long r = syscall2(SYS_ARCH_PRCTL, ARCH_SET_FS, (long)&dummy);
    test_result("arch_prctl(ARCH_SET_FS) returns 0", r == 0);

    long fs_val = 0;
    r = syscall2(SYS_ARCH_PRCTL, ARCH_GET_FS, (long)&fs_val);
    test_result("arch_prctl(ARCH_GET_FS) returns 0", r == 0);
    test_result("arch_prctl: FS base is the value we set", fs_val == (long)&dummy);
}

/* ── read from /dev/urandom ──────────────────────────────────────────── */
static void test_urandom(void) {
    int fd = (int)syscall3(SYS_OPEN, (long)"/dev/urandom", O_RDONLY, 0);
    int ok = (fd >= 0);
    if (ok) {
        unsigned char buf[8];
        long r = syscall3(SYS_READ, fd, (long)buf, 8);
        ok = (r == 8);
        test_result("read(/dev/urandom, 8) returns 8", ok);
        /* Show first 4 bytes as hex */
        if (ok) {
            print("    rand=");
            print_hex(((unsigned long)buf[0] << 24) | ((unsigned long)buf[1] << 16) |
                      ((unsigned long)buf[2] << 8)  |  (unsigned long)buf[3]);
            print("\n");
        }
        syscall1(SYS_CLOSE, fd);
    } else {
        test_result("open(/dev/urandom)", 0);
    }
}

/* ── uname / getrandom ──────────────────────────────────────────────── */
static void test_uname_getrandom(void) {
    struct utsname u;
    my_memset(&u, 0, sizeof(u));
    long r = syscall1(SYS_UNAME, (long)&u);
    test_result("uname() returns 0", r == 0);
    test_result("uname().machine is non-empty", u.machine[0] != '\0');

    unsigned char a[16], b[16];
    my_memset(a, 0, sizeof(a));
    my_memset(b, 0, sizeof(b));
    long ra = syscall3(SYS_GETRANDOM, (long)a, sizeof(a), 0);
    long rb = syscall3(SYS_GETRANDOM, (long)b, sizeof(b), 0);
    test_result("getrandom(16) returns 16", ra == 16 && rb == 16);

    int different = 0;
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i]) { different = 1; break; }
    }
    test_result("getrandom outputs vary", different);
}

static void test_rlimits(void) {
    struct rlimit rl;
    my_memset(&rl, 0, sizeof(rl));
    long r = syscall2(SYS_GETRLIMIT, RLIMIT_NOFILE, (long)&rl);
    test_result("getrlimit(RLIMIT_NOFILE) returns 0", r == 0);
    test_result("getrlimit returns sane nofile", rl.rlim_cur > 0 && rl.rlim_max >= rl.rlim_cur);

    struct rlimit old;
    my_memset(&old, 0, sizeof(old));
    r = syscall4(SYS_PRLIMIT64, 0, RLIMIT_NOFILE, 0, (long)&old);
    test_result("prlimit64(self, RLIMIT_NOFILE, NULL, old) returns 0", r == 0);
    test_result("prlimit64 old matches getrlimit", old.rlim_cur == rl.rlim_cur && old.rlim_max == rl.rlim_max);

    struct rlimit bad = { .rlim_cur = old.rlim_max + 1, .rlim_max = old.rlim_max };
    r = syscall4(SYS_PRLIMIT64, 0, RLIMIT_NOFILE, (long)&bad, 0);
    test_result("prlimit64 rejects cur > max", r == -EINVAL);
}

/* ── Multiple opens / fd exhaustion safety ───────────────────────────── */
static void test_fd_limits(void) {
    int fds[10];
    int opened = 0;
    for (int i = 0; i < 10; i++) {
        fds[i] = (int)syscall3(SYS_OPEN, (long)"/dev/null", O_RDONLY, 0);
        if (fds[i] >= 0) opened++;
    }
    test_result("open 10 fds simultaneously", opened == 10);
    for (int i = 0; i < 10; i++) {
        if (fds[i] >= 0) syscall1(SYS_CLOSE, fds[i]);
    }
}

/* ── errno ABI conformance ───────────────────────────────────────────── */
static void test_errno_contract(void) {
    long r = syscall1(SYS_CLOSE, -1);
    test_result("errno ABI: close(-1) returns -EBADF", r == -EBADF);
}

/* ═══════════════════════════════════════════════════════════════════════ */

__attribute__((force_align_arg_pointer))
void _start(void) {
    println("\033[1;36m\n  === EXO_OS Syscall Test Suite ===\033[0m\n");

    test_write();

    println("\n\033[1;33m  -- File I/O --\033[0m");
    test_open_close();
    test_read_zero();
    test_urandom();
    test_pipe();
    test_file_rw();
    test_rename();
    test_lseek();

    println("\n\033[1;33m  -- Stats --\033[0m");
    test_stat();
    test_fstat();
    test_lstat();
    test_fstatat();
    test_at_family();

    println("\n\033[1;33m  -- Directories --\033[0m");
    test_getcwd();
    test_dir_ops();
    test_getdents64();

    println("\n\033[1;33m  -- Memory --\033[0m");
    test_mmap_munmap();
    test_mmap_file_private();
    test_mprotect();
    test_brk();

    println("\n\033[1;33m  -- Process --\033[0m");
    test_pid();
    test_tid_and_tgkill();
    test_ids();
    test_set_tid_address();
    test_robust_list();
    test_futex_phase2();
    test_arch_prctl();

    println("\n\033[1;33m  -- I/O misc --\033[0m");
    test_writev();
    test_readv();
    test_pread_pwrite();
    test_access();
    test_dup_dup2();
    test_dup3();
    test_fcntl();
    test_poll_phase3();
    test_ioctl_phase3();
    test_umask();
    test_uname_getrandom();
    test_rlimits();
    test_clock_gettime();
    test_gettimeofday();
    test_times();
    test_sched_yield();
    test_nanosleep();
    test_clock_nanosleep();
    test_fd_limits();
    test_errno_contract();

    /* Summary */
    println("\n\033[1;36m  === Results ===\033[0m");
    print("  Passed: "); print_long(tests_passed);
    print(" / "); print_long(tests_run); println("");

    if (tests_passed == tests_run)
        println("  \033[1;32mAll tests PASSED!\033[0m\n");
    else
        println("  \033[1;31mSome tests FAILED.\033[0m\n");

    syscall1(SYS_EXIT_GROUP, 0);
    for (;;) __asm__ volatile("hlt");
}
