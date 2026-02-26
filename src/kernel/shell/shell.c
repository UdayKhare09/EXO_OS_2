/* shell/shell.c — Simple interactive kernel shell
 *
 * Bound to the global fbcon text console.
 * One instance lives for the lifetime of the kernel.
 */
#include "shell.h"
#include "cputest.h"
#include "gfx/fbcon.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "sched/sched.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/cpu.h"
#include "fs/vfs.h"
#include "drivers/net/netdev.h"
#include "net/netutil.h"
#include "net/arp.h"
#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/udp.h"
#include "net/tcp.h"

/* ── Line buffer ──────────────────────────────────────────────────────────── */
#define LINE_MAX 256

struct shell {
    fbcon_t *con;
    char     line[LINE_MAX];
    int      len;
    char     cwd[VFS_MOUNT_PATH_MAX];
};

/* ── ANSI helpers ─────────────────────────────────────────────────────────── */
#define A_RESET  "\033[0m"
#define A_BOLD   "\033[1m"
#define A_GREEN  "\033[1;32m"
#define A_CYAN   "\033[1;36m"
#define A_RED    "\033[1;31m"
#define A_YELLOW "\033[1;33m"
#define A_WHITE  "\033[1;37m"

/* ── Path helpers ─────────────────────────────────────────────────────────── */
static void resolve_path(shell_t *sh, const char *arg, char *out) {
    if (arg[0] == '/') path_normalize(arg, out);
    else               path_join(sh->cwd, arg, out);
}

/* ── Built-in commands ────────────────────────────────────────────────────── */
static void cmd_help(fbcon_t *c) {
    fbcon_puts_inst(c,
        A_CYAN "  Built-in commands:" A_RESET "\n"
        "    " A_BOLD "help"          A_RESET "           show this help\n"
        "    " A_BOLD "clear"         A_RESET "          clear the terminal\n"
        "    " A_BOLD "echo <text>"   A_RESET "    print text to screen\n"
        "    " A_BOLD "ver"           A_RESET "            show OS version\n"
        "    " A_BOLD "mem"           A_RESET "            physical memory stats\n"
        "    " A_BOLD "uname"         A_RESET "          alias for ver\n"
        "    " A_BOLD "sysinfo"       A_RESET "        CPU / SMP / feature scan\n"
        "    " A_BOLD "tasks"         A_RESET "          list all scheduler tasks\n"
        "    " A_BOLD "cputest"       A_RESET "        run CPU/SMP/sched test suite\n"
        A_CYAN "  Filesystem:" A_RESET "\n"
        "    " A_BOLD "ls [path]"     A_RESET "      list directory\n"
        "    " A_BOLD "cd <path>"     A_RESET "      change directory\n"
        "    " A_BOLD "pwd"           A_RESET "            print working directory\n"
        "    " A_BOLD "cat <file>"    A_RESET "     display file contents\n"
        "    " A_BOLD "stat <path>"   A_RESET "    show file/dir info\n"
        "    " A_BOLD "mkdir <path>"  A_RESET "   create directory\n"
        "    " A_BOLD "touch <file>"  A_RESET "   create empty file\n"
        A_CYAN "  Networking:" A_RESET "\n"
        "    " A_BOLD "ifconfig"                  A_RESET "  show network interfaces\n"
        "    " A_BOLD "ping <ip>"                 A_RESET "  send ICMP echo request\n"
        "    " A_BOLD "arp"                        A_RESET "  show ARP cache\n"
        "    " A_BOLD "udptest <ip> <port> [msg]" A_RESET "  send a UDP datagram and print reply\n"
        "    " A_BOLD "tcpconn <ip> <port>"       A_RESET "  open a TCP connection and exchange data\n"
        "\n"
    );
}

static void cmd_clear(fbcon_t *c) {
    fbcon_puts_inst(c, "\033[2J\033[H");
}

static void cmd_echo(fbcon_t *c, const char *args) {
    fbcon_puts_inst(c, args);
    fbcon_putchar_inst(c, '\n');
}

static void cmd_ver(fbcon_t *c) {
    fbcon_printf_inst(c,
        A_WHITE "  EXO_OS" A_RESET " 0.1.0  "
        A_CYAN  "x86_64" A_RESET "  "
        "SMP (%lu CPUs) | xHCI USB | Limine GOP\n"
        "  Clang/LLD toolchain  |  Limine 8 bootloader\n",
        (uint64_t)smp_cpu_count());
}

static void cmd_mem(fbcon_t *c) {
    (void)c;
    pmm_print_stats();
}

/* ── sysinfo — full CPU / SMP scan ───────────────────────────────────────── */
static void cmd_sysinfo(fbcon_t *c) {
    uint32_t eax, ebx, ecx, edx;

    /* ── Vendor string ────────────────────────────────────────────────────── */
    char vendor[13];
    cpuid(0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;
    /* EBX:EDX:ECX in order */
    ((uint32_t *)vendor)[0] = ebx;
    ((uint32_t *)vendor)[1] = edx;
    ((uint32_t *)vendor)[2] = ecx;
    vendor[12] = '\0';

    /* ── Brand string (leaves 0x80000002-4) ─────────────────────────────── */
    char brand[49];
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    uint32_t max_ext = eax;
    if (max_ext >= 0x80000004) {
        uint32_t *bp = (uint32_t *)brand;
        cpuid(0x80000002, &bp[0],  &bp[1],  &bp[2],  &bp[3]);
        cpuid(0x80000003, &bp[4],  &bp[5],  &bp[6],  &bp[7]);
        cpuid(0x80000004, &bp[8],  &bp[9],  &bp[10], &bp[11]);
        brand[48] = '\0';
        /* trim leading spaces */
        char *b = brand;
        while (*b == ' ') b++;
        fbcon_printf_inst(c, "\n  " A_WHITE "CPU" A_RESET ":  %s\n", b);
    } else {
        fbcon_printf_inst(c, "\n  " A_WHITE "CPU" A_RESET ":  %s (no brand string)\n", vendor);
    }
    fbcon_printf_inst(c, "  " A_WHITE "Vendor" A_RESET ": %s   CPUID max leaf: 0x%x\n",
                      vendor, max_leaf);

    /* ── Signature + feature flags ───────────────────────────────────────── */
    if (max_leaf >= 1) {
        cpuid(1, &eax, &ebx, &ecx, &edx);
        uint32_t family  = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
        uint32_t model   = ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);
        uint32_t step    = eax & 0xF;
        uint32_t logical = (ebx >> 16) & 0xFF;   /* HTT logical CPUs per pkg */

        fbcon_printf_inst(c,
            "  " A_WHITE "Family" A_RESET ": %u  "
            A_WHITE "Model" A_RESET ": %u  "
            A_WHITE "Step" A_RESET ": %u\n",
            family, model, step);

        /* Feature bits */
        fbcon_puts_inst(c, "  " A_WHITE "Features" A_RESET ": ");
        if (edx & (1<<0))  fbcon_puts_inst(c, "FPU ");
        if (edx & (1<<4))  fbcon_puts_inst(c, "TSC ");
        if (edx & (1<<5))  fbcon_puts_inst(c, "MSR ");
        if (edx & (1<<9))  fbcon_puts_inst(c, "APIC ");
        if (edx & (1<<15)) fbcon_puts_inst(c, "CMOV ");
        if (edx & (1<<19)) fbcon_puts_inst(c, "CLFL ");
        if (edx & (1<<23)) fbcon_puts_inst(c, "MMX ");
        if (edx & (1<<25)) fbcon_puts_inst(c, "SSE ");
        if (edx & (1<<26)) fbcon_puts_inst(c, "SSE2 ");
        if (edx & (1<<28)) fbcon_puts_inst(c, "HTT ");
        if (ecx & (1<<0))  fbcon_puts_inst(c, "SSE3 ");
        if (ecx & (1<<9))  fbcon_puts_inst(c, "SSSE3 ");
        if (ecx & (1<<19)) fbcon_puts_inst(c, "SSE4.1 ");
        if (ecx & (1<<20)) fbcon_puts_inst(c, "SSE4.2 ");
        if (ecx & (1<<21)) fbcon_puts_inst(c, "x2APIC ");
        if (ecx & (1<<28)) fbcon_puts_inst(c, "AVX ");
        if (ecx & (1<<30)) fbcon_puts_inst(c, "RDRND ");
        fbcon_putchar_inst(c, '\n');

        bool htt = !!(edx & (1 << 28));
        fbcon_printf_inst(c,
            "  " A_WHITE "HTT" A_RESET ": %s  "
            A_WHITE "Logical CPUs/pkg" A_RESET ": %u\n",
            htt ? A_GREEN "yes" A_RESET : A_YELLOW "no" A_RESET,
            logical);
    }

    /* ── Physical core count (leaf 4 / 0xB / extended) ─────────────────── */
    if (max_leaf >= 4) {
        cpuid(4, &eax, &ebx, &ecx, &edx);
        uint32_t phys_cores = ((eax >> 26) & 0x3F) + 1;
        fbcon_printf_inst(c,
            "  " A_WHITE "Physical cores/pkg" A_RESET ": %u\n", phys_cores);
    }

    /* ── L1/L2 cache info (leaf 0x80000005/6) ───────────────────────────── */
    if (max_ext >= 0x80000006) {
        cpuid(0x80000006, &eax, &ebx, &ecx, &edx);
        uint32_t l2_kb   = (ecx >> 16) & 0xFFFF;
        uint32_t l2_ways = (ecx >> 12) & 0xF;
        fbcon_printf_inst(c,
            "  " A_WHITE "L2 cache" A_RESET ": %u KiB  assoc=%u-way\n",
            l2_kb, l2_ways);
    }

    /* ── APIC timer calibration ──────────────────────────────────────────── */
    extern uint32_t g_apic_ticks_per_ms;
    uint32_t tpm = g_apic_ticks_per_ms;
    /* Estimate CPU MHz: tpm * 1000 / timer_divisor (divisor=16 in apic.c)  */
    /* We just report ticks/ms as proxy for LAPIC bus frequency             */
    fbcon_printf_inst(c,
        "  " A_WHITE "APIC timer" A_RESET ": %lu ticks/ms (~%lu MHz bus)\n",
        (unsigned long)tpm, (unsigned long)(tpm / 1000UL));

    /* ── SMP / per-CPU status ────────────────────────────────────────────── */
    uint32_t ncpus = smp_cpu_count();
    fbcon_printf_inst(c,
        "\n  " A_CYAN "SMP" A_RESET ": %u CPU(s) online\n", ncpus);
    fbcon_puts_inst(c,
        "  " A_WHITE "CPU" A_RESET "  "
        A_WHITE "LAPIC" A_RESET "  "
        A_WHITE "Status" A_RESET "\n"
        "  " A_YELLOW "---  -----  ------" A_RESET "\n");
    for (uint32_t i = 0; i < ncpus && i < MAX_CPUS; i++) {
        /* We don't have a smp_get_cpu_info(i) accessor, so print what we   */
        /* know: CPU index + running status (all online since boot)         */
        fbcon_printf_inst(c,
            "  %3u  %5u  " A_GREEN "online" A_RESET "\n",
            i, i /* LAPIC id matched seq on QEMU */);
    }
    fbcon_putchar_inst(c, '\n');
}

/* ── tasks — live scheduler task snapshot ────────────────────────────────── */
static void cmd_tasks(fbcon_t *c) {
    #define MAX_SNAP 32
    sched_task_info_t snap[MAX_SNAP];
    int n = sched_snapshot_tasks(snap, MAX_SNAP);

    static const char *state_str[] = {
        "RUNNABLE", "RUNNING ", "BLOCKED ", "DEAD    ", "SLEEPING"
    };
    static const char *state_col[] = {
        A_CYAN, A_GREEN, A_YELLOW, A_RED, A_YELLOW
    };

    fbcon_printf_inst(c, "\n  " A_WHITE "%-5s  %-4s  %-4s  %-8s  %s" A_RESET "\n",
                      "TID", "CPU", "PRI", "STATE", "NAME");
    fbcon_puts_inst(c, "  " A_YELLOW
                    "-----  ----  ----  --------  ----------------" A_RESET "\n");

    for (int i = 0; i < n; i++) {
        uint8_t st = snap[i].state;
        if (st > 4) st = 4;
        fbcon_printf_inst(c,
            "  %5u  %4u  %4u  %s%s" A_RESET "  %s\n",
            snap[i].tid,
            snap[i].cpu_id,
            snap[i].priority,
            state_col[st], state_str[st],
            snap[i].name);
    }
    fbcon_printf_inst(c, "\n  %d task(s) total\n\n", n);
}

/* ── Filesystem commands ──────────────────────────────────────────────────── */
static void cmd_pwd(shell_t *sh) {
    fbcon_printf_inst(sh->con, "  %s\n", sh->cwd);
}

static void cmd_cd(shell_t *sh, const char *arg) {
    if (!arg || !*arg) arg = "/";
    char path[VFS_MOUNT_PATH_MAX];
    resolve_path(sh, arg, path);

    int err = 0;
    vnode_t *v = vfs_lookup(path, true, &err);
    if (!v) {
        fbcon_printf_inst(sh->con, A_RED "  cd: %s: not found\n" A_RESET, path);
        return;
    }
    if (!VFS_S_ISDIR(v->mode)) {
        fbcon_printf_inst(sh->con, A_RED "  cd: %s: not a directory\n" A_RESET, path);
        vfs_vnode_put(v);
        return;
    }
    vfs_vnode_put(v);
    strncpy(sh->cwd, path, VFS_MOUNT_PATH_MAX - 1);
}

static void cmd_ls(shell_t *sh, const char *arg) {
    char path[VFS_MOUNT_PATH_MAX];
    if (!arg || !*arg) strncpy(path, sh->cwd, VFS_MOUNT_PATH_MAX);
    else               resolve_path(sh, arg, path);

    int err = 0;
    vnode_t *dir = vfs_lookup(path, true, &err);
    if (!dir) {
        fbcon_printf_inst(sh->con, A_RED "  ls: %s: not found\n" A_RESET, path);
        return;
    }
    if (!VFS_S_ISDIR(dir->mode)) {
        fbcon_printf_inst(sh->con, A_RED "  ls: %s: not a directory\n" A_RESET, path);
        vfs_vnode_put(dir);
        return;
    }

    vfs_dirent_t de;
    uint64_t cookie = 0;
    int count = 0;
    while (dir->ops && dir->ops->readdir &&
           dir->ops->readdir(dir, &cookie, &de) > 0) {
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) continue;
        if (de.type == VFS_DT_DIR)
            fbcon_printf_inst(sh->con, "  " A_CYAN "%s/" A_RESET "\n", de.name);
        else
            fbcon_printf_inst(sh->con, "  %s\n", de.name);
        count++;
    }
    if (count == 0)
        fbcon_puts_inst(sh->con, "  " A_YELLOW "(empty)" A_RESET "\n");
    vfs_vnode_put(dir);
}

static void cmd_cat(shell_t *sh, const char *arg) {
    if (!arg || !*arg) { fbcon_puts_inst(sh->con, A_RED "  cat: missing filename\n" A_RESET); return; }
    char path[VFS_MOUNT_PATH_MAX];
    resolve_path(sh, arg, path);

    int err = 0;
    vnode_t *v = vfs_lookup(path, true, &err);
    if (!v) { fbcon_printf_inst(sh->con, A_RED "  cat: %s: not found\n" A_RESET, path); return; }
    if (VFS_S_ISDIR(v->mode)) {
        fbcon_printf_inst(sh->con, A_RED "  cat: %s: is a directory\n" A_RESET, path);
        vfs_vnode_put(v); return;
    }
    if (v->ops && v->ops->open) v->ops->open(v, O_RDONLY);

    char buf[513];
    uint64_t off = 0;
    for (;;) {
        ssize_t n = 0;
        if (v->ops && v->ops->read) n = v->ops->read(v, buf, 512, off);
        if (n <= 0) break;
        buf[n] = '\0';
        fbcon_puts_inst(sh->con, buf);
        off += (uint64_t)n;
    }
    if (off > 0) fbcon_putchar_inst(sh->con, '\n');
    if (v->ops && v->ops->close) v->ops->close(v);
    vfs_vnode_put(v);
}

static void cmd_stat(shell_t *sh, const char *arg) {
    if (!arg || !*arg) { fbcon_puts_inst(sh->con, A_RED "  stat: missing path\n" A_RESET); return; }
    char path[VFS_MOUNT_PATH_MAX];
    resolve_path(sh, arg, path);

    vfs_stat_t st;
    int r = vfs_stat(path, &st);
    if (r < 0) { fbcon_printf_inst(sh->con, A_RED "  stat: %s: not found\n" A_RESET, path); return; }

    const char *type = "unknown";
    if      (VFS_S_ISDIR(st.mode)) type = "directory";
    else if (VFS_S_ISREG(st.mode)) type = "regular file";
    else if (VFS_S_ISLNK(st.mode)) type = "symlink";
    fbcon_printf_inst(sh->con,
        "  File: %s\n"
        "  Type: %s  Ino: %lu  Size: %lu\n"
        "  Mode: %04o  Links: %u\n",
        path, type,
        (uint64_t)st.ino, (uint64_t)st.size,
        (unsigned)(st.mode & 07777), (unsigned)st.nlink);
}

static void cmd_mkdir_shell(shell_t *sh, const char *arg) {
    if (!arg || !*arg) { fbcon_puts_inst(sh->con, A_RED "  mkdir: missing path\n" A_RESET); return; }
    char path[VFS_MOUNT_PATH_MAX];
    resolve_path(sh, arg, path);
    int r = vfs_mkdir(path, 0755);
    if (r < 0) fbcon_printf_inst(sh->con, A_RED "  mkdir: %s: error %d\n" A_RESET, path, r);
}

static void cmd_touch(shell_t *sh, const char *arg) {
    if (!arg || !*arg) { fbcon_puts_inst(sh->con, A_RED "  touch: missing filename\n" A_RESET); return; }
    char path[VFS_MOUNT_PATH_MAX];
    resolve_path(sh, arg, path);

    int err = 0;
    vnode_t *v = vfs_lookup(path, true, &err);
    if (v) { vfs_vnode_put(v); return; }

    char dir[VFS_MOUNT_PATH_MAX], name[VFS_NAME_MAX + 1];
    path_dirname(path, dir, sizeof(dir));
    path_basename(path, name, sizeof(name));

    vnode_t *parent = vfs_lookup(dir, true, &err);
    if (!parent) { fbcon_printf_inst(sh->con, A_RED "  touch: %s: parent not found\n" A_RESET, dir); return; }
    vnode_t *nv = NULL;
    if (parent->ops && parent->ops->create)
        nv = parent->ops->create(parent, name, VFS_S_IFREG | 0644);
    if (!nv) fbcon_printf_inst(sh->con, A_RED "  touch: %s: cannot create\n" A_RESET, path);
    else     vfs_vnode_put(nv);
    vfs_vnode_put(parent);
}

/* ── Networking commands ──────────────────────────────────────────────────── */

/* forward declaration — defined below with the ping helpers */
static uint32_t parse_ipv4(const char *s);

/* ---- UDP test ------------------------------------------------------------- */
static volatile int g_udp_got = 0;
static char         g_udp_rx_buf[512];
static size_t       g_udp_rx_len = 0;

static void udp_reply_cb(skbuff_t *skb,
                          uint32_t src_ip, uint16_t src_port,
                          uint32_t dst_ip, uint16_t dst_port,
                          const void *payload, size_t payload_len,
                          void *ctx)
{
    (void)skb; (void)src_ip; (void)src_port;
    (void)dst_ip; (void)dst_port; (void)ctx;
    size_t n = payload_len < sizeof(g_udp_rx_buf) - 1
               ? payload_len : sizeof(g_udp_rx_buf) - 1;
    memcpy(g_udp_rx_buf, payload, n);
    g_udp_rx_buf[n] = '\0';
    g_udp_rx_len = n;
    __atomic_store_n(&g_udp_got, 1, __ATOMIC_RELEASE);
}

static void cmd_udptest(shell_t *sh, const char *arg)
{
    fbcon_t *c = sh->con;
    if (!arg || !*arg) {
        fbcon_puts_inst(c, A_RED "  udptest: usage: udptest <ip> <port> [msg]\n" A_RESET);
        return;
    }

    uint32_t dst_ip = parse_ipv4(arg);

    /* skip to port */
    const char *p = arg;
    while (*p && *p != ' ') p++;
    while (*p == ' ')        p++;
    if (!*p) {
        fbcon_puts_inst(c, A_RED "  udptest: missing port\n" A_RESET);
        return;
    }
    uint16_t dst_port = 0;
    while (*p >= '0' && *p <= '9') dst_port = (uint16_t)(dst_port * 10 + (*p++ - '0'));
    while (*p == ' ') p++;

    const char *msg  = *p ? p : "EXO ping";
    size_t      mlen = strlen(msg);

    netdev_t *dev = netdev_get_nth(0);
    if (!dev) {
        fbcon_puts_inst(c, A_RED "  udptest: no network interface\n" A_RESET);
        return;
    }

    uint16_t src_port = udp_alloc_ephemeral();
    __atomic_store_n(&g_udp_got, 0, __ATOMIC_RELEASE);
    g_udp_rx_len = 0;

    if (udp_bind_port(src_port, 0, udp_reply_cb, NULL) < 0) {
        fbcon_puts_inst(c, A_RED "  udptest: bind failed\n" A_RESET);
        return;
    }

    fbcon_printf_inst(c, "  UDP → %d.%d.%d.%d:%u  src_port=%u  msg=\"%s\"\n",
                      IP4_A(dst_ip), IP4_B(dst_ip),
                      IP4_C(dst_ip), IP4_D(dst_ip),
                      dst_port, src_port, msg);

    int rc = udp_tx(dev, dst_ip, src_port, dst_port, msg, mlen);
    if (rc < 0) {
        fbcon_printf_inst(c, A_RED "  udp_tx failed: %d\n" A_RESET, rc);
        udp_unbind_port(src_port);
        return;
    }

    /* wait up to 2 s for a reply (20 × 100 ms) */
    for (int i = 0; i < 20; i++) {
        sched_sleep(100);
        if (__atomic_load_n(&g_udp_got, __ATOMIC_ACQUIRE)) break;
    }

    if (__atomic_load_n(&g_udp_got, __ATOMIC_ACQUIRE))
        fbcon_printf_inst(c, "  reply (%zu bytes): %s\n",
                          g_udp_rx_len, g_udp_rx_buf);
    else
        fbcon_puts_inst(c, "  no reply (timeout)\n");

    udp_unbind_port(src_port);
}

/* ---- TCP connect test ----------------------------------------------------- */
static void cmd_tcpconn(shell_t *sh, const char *arg)
{
    fbcon_t *c = sh->con;
    if (!arg || !*arg) {
        fbcon_puts_inst(c, A_RED "  tcpconn: usage: tcpconn <ip> <port>\n" A_RESET);
        return;
    }

    uint32_t dst_ip = parse_ipv4(arg);

    const char *p = arg;
    while (*p && *p != ' ') p++;
    while (*p == ' ')        p++;
    if (!*p) {
        fbcon_puts_inst(c, A_RED "  tcpconn: missing port\n" A_RESET);
        return;
    }
    uint16_t dst_port = 0;
    while (*p >= '0' && *p <= '9') dst_port = (uint16_t)(dst_port * 10 + (*p++ - '0'));

    uint32_t  next_hop = 0;
    netdev_t *dev      = ip_route(dst_ip, &next_hop);
    if (!dev) {
        fbcon_puts_inst(c, A_RED "  tcpconn: no route to host\n" A_RESET);
        return;
    }

    tcp_tcb_t *tcb = tcp_tcb_alloc();
    if (!tcb) {
        fbcon_puts_inst(c, A_RED "  tcpconn: no free TCBs\n" A_RESET);
        return;
    }

    tcb->dev         = dev;
    tcb->local_ip    = dev->ip_addr;
    tcb->local_port  = tcp_alloc_ephemeral();
    tcb->remote_ip   = dst_ip;
    tcb->remote_port = dst_port;
    tcb->snd_nxt     = tcb->iss;
    tcb->snd_una     = tcb->iss;
    tcb->state       = TCP_SYN_SENT;

    fbcon_printf_inst(c, "  TCP → %d.%d.%d.%d:%u  (src_port=%u)  SYN sent ...\n",
                      IP4_A(dst_ip), IP4_B(dst_ip),
                      IP4_C(dst_ip), IP4_D(dst_ip),
                      dst_port, tcb->local_port);

    tcp_send_segment(tcb, TCP_SYN, NULL, 0);
    tcp_rexmit_timer_reset(tcb);

    /* wait up to 5 s for ESTABLISHED (50 × 100 ms) */
    for (int i = 0; i < 50; i++) {
        sched_sleep(100);
        tcp_state_t s = __atomic_load_n(&tcb->state, __ATOMIC_ACQUIRE);
        if (s == TCP_ESTABLISHED || s == TCP_CLOSED)
            break;
    }

    if (__atomic_load_n(&tcb->state, __ATOMIC_ACQUIRE) != TCP_ESTABLISHED) {
        fbcon_printf_inst(c, A_RED "  connect timeout (state=%s)\n" A_RESET,
                          tcp_state_name(tcb->state));
        tcp_tcb_free(tcb);
        return;
    }

    fbcon_puts_inst(c, "  " A_GREEN "ESTABLISHED" A_RESET " — sending greeting\n");

    static const char greeting[] = "Hello from EXO_OS!\r\n";
    tcp_send_segment(tcb, TCP_PSH | TCP_ACK, greeting, sizeof(greeting) - 1);

    /* wait up to 3 s for incoming data (30 × 100 ms) */
    for (int i = 0; i < 30; i++) {
        sched_sleep(100);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        tcp_state_t s = __atomic_load_n(&tcb->state, __ATOMIC_ACQUIRE);
        if (tcp_rx_available(tcb) > 0 ||
            s == TCP_CLOSE_WAIT ||
            s == TCP_CLOSED)
            break;
    }

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint32_t avail = tcp_rx_available(tcb);
    if (avail > 0) {
        char buf[256];
        uint32_t n = avail > 255 ? 255 : avail;
        for (uint32_t i = 0; i < n; i++) {
            buf[i] = (char)tcb->rx_buf[tcb->rx_tail % tcb->rx_size];
            tcb->rx_tail++;
        }
        buf[n] = '\0';
        fbcon_printf_inst(c, "  data (%u bytes): %s\n", n, buf);
    } else {
        fbcon_puts_inst(c, "  (no data received)\n");
    }

    /* graceful close */
    tcp_send_segment(tcb, TCP_FIN | TCP_ACK, NULL, 0);
    sched_sleep(500);
    tcp_tcb_free(tcb);
}

static void cmd_ifconfig(fbcon_t *c) {
    int count = netdev_count();
    if (count == 0) {
        fbcon_puts_inst(c, "  No network interfaces.\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        netdev_t *dev = netdev_get_nth(i);
        if (!dev) continue;
        fbcon_printf_inst(c,
            "  %s: link=%s mtu=%u\n"
            "    HWaddr %02x:%02x:%02x:%02x:%02x:%02x\n"
            "    inet %d.%d.%d.%d  mask %d.%d.%d.%d  gw %d.%d.%d.%d\n"
            "    dns %d.%d.%d.%d\n\n",
            dev->name, dev->link ? "up" : "down", dev->mtu,
            dev->mac[0], dev->mac[1], dev->mac[2],
            dev->mac[3], dev->mac[4], dev->mac[5],
            IP4_A(dev->ip_addr), IP4_B(dev->ip_addr),
            IP4_C(dev->ip_addr), IP4_D(dev->ip_addr),
            IP4_A(dev->netmask), IP4_B(dev->netmask),
            IP4_C(dev->netmask), IP4_D(dev->netmask),
            IP4_A(dev->gateway), IP4_B(dev->gateway),
            IP4_C(dev->gateway), IP4_D(dev->gateway),
            IP4_A(dev->dns), IP4_B(dev->dns),
            IP4_C(dev->dns), IP4_D(dev->dns));
    }
}

/* simple IP parser: "A.B.C.D" → host-order uint32 */
static uint32_t parse_ipv4(const char *s) {
    uint32_t a = 0, b = 0, c2 = 0, d = 0;
    int pos = 0;
    uint32_t *cur = &a;
    while (s[pos]) {
        if (s[pos] >= '0' && s[pos] <= '9') {
            *cur = *cur * 10 + (s[pos] - '0');
        } else if (s[pos] == '.') {
            if (cur == &a) cur = &b;
            else if (cur == &b) cur = &c2;
            else if (cur == &c2) cur = &d;
        } else {
            break;  /* stop at space, NUL, or any non-IP character */
        }
        pos++;
    }
    return IP4_ADDR((uint8_t)a, (uint8_t)b, (uint8_t)c2, (uint8_t)d);
}

static void cmd_ping(shell_t *sh, const char *arg) {
    fbcon_t *c = sh->con;
    if (!arg || !*arg) {
        fbcon_puts_inst(c, A_RED "  ping: missing IP address\n" A_RESET);
        return;
    }
    uint32_t dst = parse_ipv4(arg);
    netdev_t *dev = netdev_get_nth(0);
    if (!dev) {
        fbcon_puts_inst(c, A_RED "  ping: no network interface\n" A_RESET);
        return;
    }

    fbcon_printf_inst(c, "  PING %d.%d.%d.%d ...\n",
                     IP4_A(dst), IP4_B(dst), IP4_C(dst), IP4_D(dst));

    for (int i = 0; i < 4; i++) {
        int rc = icmp_send_echo(dev, dst, 0x1234, (uint16_t)(i + 1),
                                "exo", 3);
        if (rc < 0) {
            fbcon_puts_inst(c, "  send failed\n");
            continue;
        }
        /* wait up to 2 seconds for reply */
        for (int w = 0; w < 20; w++) {
            sched_sleep(100);
            if (g_ping_state.reply_received) break;
        }
        if (g_ping_state.reply_received) {
            fbcon_printf_inst(c, "  reply from %d.%d.%d.%d seq=%u\n",
                             IP4_A(dst), IP4_B(dst),
                             IP4_C(dst), IP4_D(dst), i + 1);
        } else {
            fbcon_printf_inst(c, "  request timeout seq=%u\n", i + 1);
        }
    }
}

static void cmd_arp_show(fbcon_t *c) {
    arp_entry_t table[64];
    int count = arp_get_table(table, 64);
    if (count == 0) {
        fbcon_puts_inst(c, "  ARP cache is empty.\n");
        return;
    }
    fbcon_puts_inst(c, "  IP Address        HW Address\n");
    for (int i = 0; i < count; i++) {
        fbcon_printf_inst(c, "  %d.%d.%d.%d     %02x:%02x:%02x:%02x:%02x:%02x\n",
                         IP4_A(table[i].ip), IP4_B(table[i].ip),
                         IP4_C(table[i].ip), IP4_D(table[i].ip),
                         table[i].mac[0], table[i].mac[1], table[i].mac[2],
                         table[i].mac[3], table[i].mac[4], table[i].mac[5]);
    }
}

/* ── Command dispatcher ───────────────────────────────────────────────────── */
static void run_line(shell_t *sh) {
    fbcon_t *c = sh->con;
    sh->line[sh->len] = '\0';
    sh->len = 0;

    char *p = sh->line;
    while (*p == ' ') p++;

    fbcon_hide_cursor_inst(c);
    fbcon_putchar_inst(c, '\n');

    if (*p == '\0') {
        /* empty */
    } else if (strcmp(p, "help") == 0) {
        fbcon_putchar_inst(c, '\n');
        cmd_help(c);
    } else if (strcmp(p, "clear") == 0) {
        cmd_clear(c);
        goto reprompt;
    } else if (strncmp(p, "echo", 4) == 0 && (p[4] == ' ' || p[4] == '\0')) {
        cmd_echo(c, p[4] == ' ' ? p + 5 : "");
    } else if (strcmp(p, "ver") == 0 || strcmp(p, "uname") == 0) {
        fbcon_putchar_inst(c, '\n'); cmd_ver(c); fbcon_putchar_inst(c, '\n');
    } else if (strcmp(p, "mem") == 0) {
        fbcon_putchar_inst(c, '\n'); cmd_mem(c); fbcon_putchar_inst(c, '\n');
    } else if (strcmp(p, "pwd") == 0) {
        cmd_pwd(sh);
    } else if (strncmp(p, "cd", 2) == 0 && (p[2] == ' ' || p[2] == '\0')) {
        cmd_cd(sh, p[2] == ' ' ? p + 3 : NULL);
    } else if (strncmp(p, "ls", 2) == 0 && (p[2] == ' ' || p[2] == '\0')) {
        cmd_ls(sh, p[2] == ' ' ? p + 3 : NULL);
    } else if (strncmp(p, "cat ", 4) == 0) {
        cmd_cat(sh, p + 4);
    } else if (strncmp(p, "stat ", 5) == 0) {
        cmd_stat(sh, p + 5);
    } else if (strncmp(p, "mkdir ", 6) == 0) {
        cmd_mkdir_shell(sh, p + 6);
    } else if (strncmp(p, "touch ", 6) == 0) {
        cmd_touch(sh, p + 6);
    } else if (strcmp(p, "cputest") == 0) {
        cmd_sysinfo(c);   /* always show sysinfo context first */
        fbcon_putchar_inst(c, '\n');
        cputest_run(c);
    } else if (strcmp(p, "sysinfo") == 0) {
        cmd_sysinfo(c);
    } else if (strcmp(p, "tasks") == 0) {
        cmd_tasks(c);
    } else if (strcmp(p, "ifconfig") == 0) {
        cmd_ifconfig(c);
    } else if (strncmp(p, "ping ", 5) == 0) {
        cmd_ping(sh, p + 5);
    } else if (strcmp(p, "arp") == 0) {
        cmd_arp_show(c);
    } else if (strncmp(p, "udptest ", 8) == 0) {
        cmd_udptest(sh, p + 8);
    } else if (strncmp(p, "tcpconn ", 8) == 0) {
        cmd_tcpconn(sh, p + 8);
    } else {
        fbcon_puts_inst(c, A_RED "  error:" A_RESET " unknown command '");
        fbcon_puts_inst(c, p);
        fbcon_puts_inst(c, "' -- try " A_BOLD "help" A_RESET "\n\n");
    }

reprompt:
    fbcon_printf_inst(c, A_GREEN "exo" A_RESET ":" A_CYAN "%s" A_RESET "> ", sh->cwd);
    fbcon_show_cursor_inst(c);
}

/* ── Public API ───────────────────────────────────────────────────────────── */
shell_t *shell_create(fbcon_t *con) {
    shell_t *sh = kzalloc(sizeof(*sh));
    if (!sh) return NULL;
    sh->con = con;
    sh->len = 0;
    strncpy(sh->cwd, "/", VFS_MOUNT_PATH_MAX);

    /* Banner */
    fbcon_puts_inst(con, "\n"
        A_CYAN "  ╔══════════════════════════════╗\n" A_RESET
        A_CYAN "  ║" A_WHITE "   EXO_OS  v0.1.0            " A_CYAN "║\n" A_RESET
        A_CYAN "  ║" A_YELLOW " UEFI/Limine GOP Console     " A_CYAN "║\n" A_RESET
        A_CYAN "  ╚══════════════════════════════╝\n" A_RESET "\n"
        "  Type " A_BOLD "help" A_RESET " to list commands.\n\n");

    fbcon_printf_inst(con, A_GREEN "exo" A_RESET ":" A_CYAN "/" A_RESET "> ");
    fbcon_show_cursor_inst(con);
    return sh;
}

void shell_destroy(shell_t *sh) {
    if (sh) kfree(sh);
}

fbcon_t *shell_get_fbcon(shell_t *sh) { return sh ? sh->con : NULL; }

void shell_on_char_inst(shell_t *sh, char c) {
    if (!sh || !sh->con) return;
    fbcon_hide_cursor_inst(sh->con);

    if (c == '\r') { /* ignore */ }
    else if (c == '\n') { run_line(sh); return; }
    else if (c == '\b') {
        if (sh->len > 0) { sh->len--; fbcon_putchar_inst(sh->con, '\b'); }
    } else if ((unsigned char)c >= 0x20 && sh->len < LINE_MAX - 1) {
        sh->line[sh->len++] = c;
        fbcon_putchar_inst(sh->con, c);
    }

    fbcon_show_cursor_inst(sh->con);
}
