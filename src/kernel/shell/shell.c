/* shell/shell.c — Interactive kernel shell with command registration
 *
 * Bound to the global fbcon text console.
 * One instance lives for the lifetime of the kernel.
 *
 * Commands are registered via shell_register_cmd() from any subsystem
 * init function.  The table is sorted once before the REPL loop starts,
 * and dispatched via binary search.
 */
#include "shell.h"
#include "cputest.h"
#include "gfx/fbcon.h"
#include "lib/string.h"
#include "lib/spinlock.h"
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
#include "fs/fd.h"
#include "fs/elf.h"
#include "sched/task.h"

/* Defined in syscall/net_syscalls.c */
extern void tty_set_fg_pgid(int pgid);
#include "mm/vmm.h"

/* ── ANSI helpers ─────────────────────────────────────────────────────────── */
#define A_RESET  "\033[0m"
#define A_BOLD   "\033[1m"
#define A_GREEN  "\033[1;32m"
#define A_CYAN   "\033[1;36m"
#define A_RED    "\033[1;31m"
#define A_YELLOW "\033[1;33m"
#define A_WHITE  "\033[1;37m"

/* ── Command registration table ───────────────────────────────────────────── */
#define SHELL_MAX_CMDS 128

static shell_cmd_t g_cmds[SHELL_MAX_CMDS];
static int         g_cmd_count = 0;
static spinlock_t  g_cmd_lock;

void shell_register_cmd(const char *name, const char *help, shell_cmd_fn_t fn) {
    spinlock_acquire(&g_cmd_lock);
    /* check for duplicate */
    for (int i = 0; i < g_cmd_count; i++) {
        if (strcmp(g_cmds[i].name, name) == 0) {
            spinlock_release(&g_cmd_lock);
            return;
        }
    }
    if (g_cmd_count < SHELL_MAX_CMDS) {
        g_cmds[g_cmd_count].name = name;
        g_cmds[g_cmd_count].help = help;
        g_cmds[g_cmd_count].fn   = fn;
        g_cmd_count++;
    }
    spinlock_release(&g_cmd_lock);
}

void shell_sort_commands(void) {
    /* simple insertion sort — called once with ~20 items */
    for (int i = 1; i < g_cmd_count; i++) {
        shell_cmd_t tmp = g_cmds[i];
        int j = i - 1;
        while (j >= 0 && strcmp(g_cmds[j].name, tmp.name) > 0) {
            g_cmds[j + 1] = g_cmds[j];
            j--;
        }
        g_cmds[j + 1] = tmp;
    }
}

static shell_cmd_t *shell_find_cmd(const char *name) {
    /* binary search on sorted table */
    int lo = 0, hi = g_cmd_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(g_cmds[mid].name, name);
        if (cmp == 0) return &g_cmds[mid];
        if (cmp < 0)  lo = mid + 1;
        else           hi = mid - 1;
    }
    return NULL;
}

/* ── exec: launch an ELF binary as a user-space process ──────────────────── */
void cmd_exec_path(shell_t *sh, const char *path);  /* forward decl */

static void cmd_exec(shell_t *sh, const char *args) {
    fbcon_t *c = sh->con;
    if (!args || !*args) {
        fbcon_puts_inst(c, "  usage: exec <path>\n");
        return;
    }

    /* Skip leading spaces */
    while (*args == ' ') args++;
    if (!*args) {
        fbcon_puts_inst(c, "  usage: exec <path>\n");
        return;
    }

    /* Resolve the path relative to CWD */
    char fullpath[VFS_MOUNT_PATH_MAX];
    if (args[0] == '/') {
        strncpy(fullpath, args, VFS_MOUNT_PATH_MAX - 1);
        fullpath[VFS_MOUNT_PATH_MAX - 1] = '\0';
    } else {
        path_join(sh->cwd, args, fullpath);
    }

    /* Open the ELF file */
    int vfs_err = 0;
    vnode_t *vn = vfs_lookup(fullpath, true, &vfs_err);
    if (!vn) {
        fbcon_printf_inst(c, "  exec: '%s': file not found\n", fullpath);
        return;
    }

    uint64_t file_size = vn->size;
    if (file_size == 0 || file_size > (64ULL * 1024 * 1024)) {
        fbcon_printf_inst(c, "  exec: '%s': invalid file size (%llu)\n", fullpath, file_size);
        vfs_vnode_put(vn);
        return;
    }

    /* Read the ELF file into a kernel buffer */
    void *buf = kmalloc(file_size);
    if (!buf) {
        fbcon_puts_inst(c, "  exec: out of memory\n");
        vfs_vnode_put(vn);
        return;
    }

    ssize_t rd = vn->ops->read(vn, buf, file_size, 0);
    vfs_vnode_put(vn);
    if (rd < 0 || (uint64_t)rd != file_size) {
        fbcon_printf_inst(c, "  exec: read error (%ld)\n", rd);
        kfree(buf);
        return;
    }

    /* Create a fresh user address space */
    uintptr_t pml4 = vmm_create_address_space();
    if (!pml4) {
        fbcon_puts_inst(c, "  exec: failed to create address space\n");
        kfree(buf);
        return;
    }

    /* Load the ELF into the new address space */
    elf_info_t info;
    int r = elf_load(buf, file_size, pml4, &info);
    kfree(buf);
    if (r < 0) {
        fbcon_printf_inst(c, "  exec: ELF load failed (%d)\n", r);
        vmm_destroy_address_space(pml4);
        return;
    }

    /* Allocate user stack (8 pages = 32 KiB) */
    uintptr_t user_stack_top = USER_STACK_TOP;
    uintptr_t stack_pages = 8;
    for (uintptr_t i = 0; i < stack_pages; i++) {
        uintptr_t pg = pmm_alloc_pages(1);
        if (pg) {
            memset((void *)vmm_phys_to_virt(pg), 0, PAGE_SIZE);
            vmm_map_page_in(pml4,
                            user_stack_top - (stack_pages - i) * PAGE_SIZE,
                            pg, VMM_USER_RW);
        }
    }

    /* Build minimal user stack: argc=1, argv[0]=path, argv[1]=NULL, envp=NULL, auxv=AT_NULL */
    uintptr_t sp = user_stack_top;

    /* We need to write into the user address space page via HHDM */
    #define EXEC_PUSH(val) do {                                          \
        sp -= 8;                                                          \
        uint64_t *_pte = vmm_get_pte(pml4, sp);                          \
        if (_pte) {                                                       \
            uintptr_t _ph = (*_pte) & 0x000FFFFFFFFFF000ULL;             \
            uintptr_t _of = sp & (PAGE_SIZE - 1);                        \
            *(uint64_t *)(vmm_phys_to_virt(_ph) + _of) = (uint64_t)(val);\
        }                                                                 \
    } while(0)

    /* Helper: copy a string to user stack, return its user address */
    #define EXEC_PUSH_STR(str) ({ \
        size_t _len = strlen(str) + 1; \
        sp -= _len; \
        sp &= ~0x7ULL; \
        uintptr_t _addr = sp; \
        uint64_t *_pte2 = vmm_get_pte(pml4, sp); \
        if (_pte2) { \
            uintptr_t _ph2 = (*_pte2) & 0x000FFFFFFFFFF000ULL; \
            uintptr_t _of2 = sp & (PAGE_SIZE - 1); \
            memcpy((void *)(vmm_phys_to_virt(_ph2) + _of2), (str), _len); \
        } \
        _addr; \
    })

    /* argv[0]: use "-sh" for /bin/sh so BusyBox ash treats it as login shell
     * and sources /etc/profile. */
    const char *argv0_str = fullpath;
    if (strcmp(fullpath, "/bin/sh") == 0)
        argv0_str = "-sh";
    uintptr_t argv0_addr = EXEC_PUSH_STR(argv0_str);

    char env_pwd[VFS_MOUNT_PATH_MAX + 5];
    const char *cwd_env = (sh && sh->cwd[0]) ? sh->cwd : "/";
    size_t cwd_len = strlen(cwd_env);
    if (cwd_len > VFS_MOUNT_PATH_MAX - 1)
        cwd_len = VFS_MOUNT_PATH_MAX - 1;
    memcpy(env_pwd, "PWD=", 4);
    memcpy(env_pwd + 4, cwd_env, cwd_len);
    env_pwd[4 + cwd_len] = '\0';

    /* Push environment strings */
    const char *env_list[] = {
        "PATH=/bin:/usr/bin:/sbin:/usr/sbin",
        "HOME=/",
        "TERM=linux",
        "SHELL=/bin/sh",
        "ENV=/etc/profile",
        "USER=root",
        "LOGNAME=root",
        env_pwd,
        "TMPDIR=/tmp",
        "PS1=# ",
    };
    uintptr_t env_addrs[sizeof(env_list) / sizeof(env_list[0])] = {0};
    int env_count = 0;
    for (size_t i = 0; i < sizeof(env_list) / sizeof(env_list[0]); i++) {
        env_addrs[env_count++] = EXEC_PUSH_STR(env_list[i]);
    }

    /* Align to 16 bytes before the pointer table */
    sp &= ~0xFULL;

    /* Auxiliary vector (match sys_execve layout) */
    EXEC_PUSH(0);                  /* AT_NULL value */
    EXEC_PUSH(0);                  /* AT_NULL type  */
    EXEC_PUSH(0);                  /* AT_SECURE value */
    EXEC_PUSH(23);                 /* AT_SECURE type  */
    EXEC_PUSH(info.entry);         /* AT_ENTRY value  */
    EXEC_PUSH(9);                  /* AT_ENTRY type   */
    EXEC_PUSH(PAGE_SIZE);          /* AT_PAGESZ value */
    EXEC_PUSH(6);                  /* AT_PAGESZ type  */
    EXEC_PUSH(info.phdr_size);     /* AT_PHENT value  */
    EXEC_PUSH(4);                  /* AT_PHENT type   */
    EXEC_PUSH(info.phdr_count);    /* AT_PHNUM value  */
    EXEC_PUSH(5);                  /* AT_PHNUM type   */
    EXEC_PUSH(info.phdr_vaddr);    /* AT_PHDR value   */
    EXEC_PUSH(3);                  /* AT_PHDR type    */

    /* envp: strings + NULL terminator */
    EXEC_PUSH(0);
    for (int i = env_count - 1; i >= 0; i--)
        EXEC_PUSH(env_addrs[i]);

    /* argv: argv[0]=path, NULL */
    EXEC_PUSH(0);
    EXEC_PUSH(argv0_addr);

    /* argc */
    EXEC_PUSH(1);

    /* Do not adjust SP after argc is pushed: process entry expects
     * argc at [RSP] followed by argv/envp/auxv contiguously. */

    #undef EXEC_PUSH_STR

    #undef EXEC_PUSH

    /* Create the user-mode task */
    uint32_t cpu = sched_pick_cpu();
    task_t *t = task_create_user(fullpath, pml4, info.entry, sp, cpu);
    if (!t) {
        fbcon_puts_inst(c, "  exec: failed to create task\n");
        vmm_destroy_address_space(pml4);
        return;
    }

    /* Set up brk */
    t->brk_base    = info.brk_start;
    t->brk_current = info.brk_start;
    t->mmap_next   = USER_MMAP_BASE;
    const char *initial_cwd = (sh && sh->cwd[0]) ? sh->cwd : "/";
    strncpy(t->cwd, initial_cwd, TASK_CWD_MAX - 1);
    t->cwd[TASK_CWD_MAX - 1] = '\0';

    /* Add stack VMA */
    vma_t *stack_vma = kmalloc(sizeof(vma_t));
    if (stack_vma) {
        stack_vma->start = user_stack_top - stack_pages * PAGE_SIZE;
        stack_vma->end   = user_stack_top;
        stack_vma->flags = VMA_READ | VMA_WRITE | VMA_USER | VMA_STACK;
        stack_vma->next  = NULL;
        vma_insert(t, stack_vma);
    }

    /* Set up stdio: fd 0/1/2 → /dev/tty */
    int vfs_err2 = 0;
    vnode_t *tty = vfs_lookup("/dev/tty", true, &vfs_err2);
    if (tty) {
        fd_setup_stdio(t, tty);
        vfs_vnode_put(tty);
    } else {
        fbcon_puts_inst(c, "  exec: warning: could not open /dev/tty for stdio\n");
    }

    /* Make this process the TTY foreground group so shell job control works */
    tty_set_fg_pgid((int)t->pgid);

    /* Set shell as parent so child becomes ZOMBIE (not immediately freed) on exit */
    t->parent = sched_current();
    uint32_t child_tid = t->tid;

    /* Schedule the task */
    sched_add_task(t, cpu);

    /* Wait for child to exit — poll every 10 ms.
     * Because t->parent is set, the child transitions to TASK_ZOMBIE on exit
     * and stays in the task table until we reap it here. */
    {
        task_t *ct;
        for (;;) {
            ct = task_lookup(child_tid);
            if (!ct || ct->state == TASK_ZOMBIE || ct->state == TASK_DEAD)
                break;
            sched_sleep(10);
        }
        /* Reap the zombie so its slot is freed */
        ct = task_lookup(child_tid);
        if (ct && ct->state == TASK_ZOMBIE) {
            ct->parent = NULL;
            task_destroy(ct);
        }
    }
}

/* ── Public API: launch a binary by path (used by init_task in main.c) ── */
void cmd_exec_path(shell_t *sh, const char *path) {
    cmd_exec(sh, path);
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
        /* empty line */
    } else {
        /* Extract command name (first word) */
        char cmd_name[64];
        int ci = 0;
        const char *q = p;
        while (*q && *q != ' ' && ci < 63) cmd_name[ci++] = *q++;
        cmd_name[ci] = '\0';

        /* Skip space after command to get args */
        while (*q == ' ') q++;
        const char *args = (*q) ? q : NULL;

        shell_cmd_t *cmd = shell_find_cmd(cmd_name);
        if (cmd) {
            cmd->fn(sh, args);
        } else {
            fbcon_puts_inst(c, A_RED "  error:" A_RESET " unknown command '");
            fbcon_puts_inst(c, p);
            fbcon_puts_inst(c, "' -- try " A_BOLD "help" A_RESET "\n\n");
        }
    }

    /* Handle 'clear' specially: don't re-print prompt prefix if screen was cleared */
    fbcon_printf_inst(c, A_GREEN "exo" A_RESET ":" A_CYAN "%s" A_RESET "> ", sh->cwd);
    fbcon_show_cursor_inst(c);
}

shell_t *shell_create_quiet(fbcon_t *con) {
    shell_t *sh = kzalloc(sizeof(*sh));
    if (!sh) return NULL;
    sh->con = con;
    sh->len = 0;
    strncpy(sh->cwd, "/", VFS_MOUNT_PATH_MAX);
    return sh;
}