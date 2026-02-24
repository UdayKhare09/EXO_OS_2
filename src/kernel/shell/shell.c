/* shell/shell.c — Instance-based interactive kernel shell
 *
 * Each shell_t has its own line buffer and is bound to an fbcon_t terminal.
 * Multiple shells can run in different terminal windows simultaneously.
 */
#include "shell.h"
#include "gfx/fbcon.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "sched/sched.h"
#include "arch/x86_64/smp.h"
#include "fs/vfs.h"

/* ── Line buffer ──────────────────────────────────────────────────────────── */
#define LINE_MAX 256

struct shell {
    fbcon_t *con;
    char     line[LINE_MAX];
    int      len;
    char     cwd[VFS_MOUNT_PATH_MAX];   /* current working directory */
};

/* ── ANSI helpers ─────────────────────────────────────────────────────────── */
#define A_RESET  "\033[0m"
#define A_BOLD   "\033[1m"
#define A_GREEN  "\033[1;32m"
#define A_CYAN   "\033[1;36m"
#define A_RED    "\033[1;31m"
#define A_YELLOW "\033[1;33m"
#define A_WHITE  "\033[1;37m"

/* ── Resolve path relative to shell cwd ───────────────────────────────────── */
static void resolve_path(shell_t *sh, const char *arg, char *out) {
    if (arg[0] == '/') {
        path_normalize(arg, out);
    } else {
        path_join(sh->cwd, arg, out);
    }
}

/* ── Built-in commands ────────────────────────────────────────────────────── */
static void cmd_help(fbcon_t *c) {
    fbcon_puts_inst(c,
        A_CYAN "  Built-in commands:" A_RESET "\n"
        "    " A_BOLD "help" A_RESET "           show this help\n"
        "    " A_BOLD "clear" A_RESET "          clear the terminal\n"
        "    " A_BOLD "echo <text>" A_RESET "    print text to screen\n"
        "    " A_BOLD "ver" A_RESET "            show OS version\n"
        "    " A_BOLD "mem" A_RESET "            physical memory stats\n"
        "    " A_BOLD "uname" A_RESET "          alias for ver\n"
        A_CYAN "  Filesystem:" A_RESET "\n"
        "    " A_BOLD "ls [path]" A_RESET "      list directory\n"
        "    " A_BOLD "cd <path>" A_RESET "      change directory\n"
        "    " A_BOLD "pwd" A_RESET "            print working directory\n"
        "    " A_BOLD "cat <file>" A_RESET "     display file contents\n"
        "    " A_BOLD "stat <path>" A_RESET "    show file/dir info\n"
        "    " A_BOLD "mkdir <path>" A_RESET "   create directory\n"
        "    " A_BOLD "touch <file>" A_RESET "   create empty file\n"
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
        "SMP (%lu CPUs) | xHCI USB | VirtIO GPU\n"
        "  Clang/LLD toolchain  |  Limine 8 bootloader\n",
        (uint64_t)smp_cpu_count());
}

static void cmd_mem(fbcon_t *c) {
    (void)c;
    pmm_print_stats();
}

/* ── Filesystem commands ──────────────────────────────────────────────────── */
static void cmd_pwd(shell_t *sh) {
    fbcon_printf_inst(sh->con, "  %s\n", sh->cwd);
}

static void cmd_cd(shell_t *sh, const char *arg) {
    if (!arg || !*arg) { arg = "/"; }
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
    if (!arg || !*arg)
        strncpy(path, sh->cwd, VFS_MOUNT_PATH_MAX);
    else
        resolve_path(sh, arg, path);

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
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
            continue;
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
    if (!arg || !*arg) {
        fbcon_puts_inst(sh->con, A_RED "  cat: missing filename\n" A_RESET);
        return;
    }
    char path[VFS_MOUNT_PATH_MAX];
    resolve_path(sh, arg, path);

    int err = 0;
    vnode_t *v = vfs_lookup(path, true, &err);
    if (!v) {
        fbcon_printf_inst(sh->con, A_RED "  cat: %s: not found\n" A_RESET, path);
        return;
    }
    if (VFS_S_ISDIR(v->mode)) {
        fbcon_printf_inst(sh->con, A_RED "  cat: %s: is a directory\n" A_RESET, path);
        vfs_vnode_put(v);
        return;
    }
    if (v->ops && v->ops->open) v->ops->open(v, O_RDONLY);

    char buf[513];
    uint64_t off = 0;
    for (;;) {
        ssize_t n = 0;
        if (v->ops && v->ops->read)
            n = v->ops->read(v, buf, 512, off);
        if (n <= 0) break;
        buf[n] = '\0';
        fbcon_puts_inst(sh->con, buf);
        off += (uint64_t)n;
    }
    /* Ensure output ends with a newline */
    if (off > 0) fbcon_putchar_inst(sh->con, '\n');

    if (v->ops && v->ops->close) v->ops->close(v);
    vfs_vnode_put(v);
}

static void cmd_stat(shell_t *sh, const char *arg) {
    if (!arg || !*arg) {
        fbcon_puts_inst(sh->con, A_RED "  stat: missing path\n" A_RESET);
        return;
    }
    char path[VFS_MOUNT_PATH_MAX];
    resolve_path(sh, arg, path);

    vfs_stat_t st;
    int r = vfs_stat(path, &st);
    if (r < 0) {
        fbcon_printf_inst(sh->con, A_RED "  stat: %s: not found\n" A_RESET, path);
        return;
    }
    const char *type = "unknown";
    if (VFS_S_ISDIR(st.mode)) type = "directory";
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
    if (!arg || !*arg) {
        fbcon_puts_inst(sh->con, A_RED "  mkdir: missing path\n" A_RESET);
        return;
    }
    char path[VFS_MOUNT_PATH_MAX];
    resolve_path(sh, arg, path);
    int r = vfs_mkdir(path, 0755);
    if (r < 0)
        fbcon_printf_inst(sh->con, A_RED "  mkdir: %s: error %d\n" A_RESET, path, r);
}

static void cmd_touch(shell_t *sh, const char *arg) {
    if (!arg || !*arg) {
        fbcon_puts_inst(sh->con, A_RED "  touch: missing filename\n" A_RESET);
        return;
    }
    char path[VFS_MOUNT_PATH_MAX];
    resolve_path(sh, arg, path);

    /* Check if already exists */
    int err = 0;
    vnode_t *v = vfs_lookup(path, true, &err);
    if (v) { vfs_vnode_put(v); return; }  /* already exists, nothing to do */

    /* Split into parent dir + filename */
    char dir[VFS_MOUNT_PATH_MAX], name[VFS_NAME_MAX + 1];
    path_dirname(path, dir, sizeof(dir));
    path_basename(path, name, sizeof(name));

    vnode_t *parent = vfs_lookup(dir, true, &err);
    if (!parent) {
        fbcon_printf_inst(sh->con, A_RED "  touch: %s: parent not found\n" A_RESET, dir);
        return;
    }
    vnode_t *nv = NULL;
    if (parent->ops && parent->ops->create)
        nv = parent->ops->create(parent, name, VFS_S_IFREG | 0644);
    if (!nv)
        fbcon_printf_inst(sh->con, A_RED "  touch: %s: cannot create\n" A_RESET, path);
    else
        vfs_vnode_put(nv);
    vfs_vnode_put(parent);
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
    } else if (strncmp(p, "echo", 4) == 0 &&
               (p[4] == ' ' || p[4] == '\0')) {
        cmd_echo(c, p[4] == ' ' ? p + 5 : "");
    } else if (strcmp(p, "ver") == 0 || strcmp(p, "uname") == 0) {
        fbcon_putchar_inst(c, '\n');
        cmd_ver(c);
        fbcon_putchar_inst(c, '\n');
    } else if (strcmp(p, "mem") == 0) {
        fbcon_putchar_inst(c, '\n');
        cmd_mem(c);
        fbcon_putchar_inst(c, '\n');
    } else if (strcmp(p, "pwd") == 0) {
        cmd_pwd(sh);
    } else if (strncmp(p, "cd", 2) == 0 &&
               (p[2] == ' ' || p[2] == '\0')) {
        cmd_cd(sh, p[2] == ' ' ? p + 3 : NULL);
    } else if (strncmp(p, "ls", 2) == 0 &&
               (p[2] == ' ' || p[2] == '\0')) {
        cmd_ls(sh, p[2] == ' ' ? p + 3 : NULL);
    } else if (strncmp(p, "cat ", 4) == 0) {
        cmd_cat(sh, p + 4);
    } else if (strncmp(p, "stat ", 5) == 0) {
        cmd_stat(sh, p + 5);
    } else if (strncmp(p, "mkdir ", 6) == 0) {
        cmd_mkdir_shell(sh, p + 6);
    } else if (strncmp(p, "touch ", 6) == 0) {
        cmd_touch(sh, p + 6);
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
               A_CYAN "  +----------------------------+" A_RESET "\n"
               A_CYAN "  |" A_WHITE "   EXO_OS  v0.1.0         " A_CYAN "|" A_RESET "\n"
               A_CYAN "  +----------------------------+" A_RESET "\n\n"
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

    if (c == '\r') {
        /* ignore */
    } else if (c == '\n') {
        run_line(sh);
        return;
    } else if (c == '\b') {
        if (sh->len > 0) {
            sh->len--;
            fbcon_putchar_inst(sh->con, '\b');
        }
    } else if ((unsigned char)c >= 0x20 && sh->len < LINE_MAX - 1) {
        sh->line[sh->len++] = c;
        fbcon_putchar_inst(sh->con, c);
    }

    fbcon_show_cursor_inst(sh->con);
}

/* ── Legacy single-instance wrappers ──────────────────────────────────────── */
static shell_t *g_legacy_shell = NULL;

void shell_init(void) {
    /* g_legacy fbcon is already init'd by fbcon_init() */
    extern fbcon_t *fbcon_create(const char *, int, int, int, int);
    /* We need to get the legacy fbcon — use a different approach:
     * shell_init() is called from desktop_task which should set up its own. */
}

void shell_on_char(char c) {
    if (g_legacy_shell) shell_on_char_inst(g_legacy_shell, c);
}
