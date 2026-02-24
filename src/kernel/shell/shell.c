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

/* ── Line buffer ──────────────────────────────────────────────────────────── */
#define LINE_MAX 256

struct shell {
    fbcon_t *con;
    char     line[LINE_MAX];
    int      len;
};

/* ── ANSI helpers ─────────────────────────────────────────────────────────── */
#define A_RESET  "\033[0m"
#define A_BOLD   "\033[1m"
#define A_GREEN  "\033[1;32m"
#define A_CYAN   "\033[1;36m"
#define A_RED    "\033[1;31m"
#define A_YELLOW "\033[1;33m"
#define A_WHITE  "\033[1;37m"

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
    } else {
        fbcon_puts_inst(c, A_RED "  error:" A_RESET " unknown command '");
        fbcon_puts_inst(c, p);
        fbcon_puts_inst(c, "' -- try " A_BOLD "help" A_RESET "\n\n");
    }

reprompt:
    fbcon_puts_inst(c, A_GREEN "exo" A_RESET "> ");
    fbcon_show_cursor_inst(c);
}

/* ── Public API ───────────────────────────────────────────────────────────── */
shell_t *shell_create(fbcon_t *con) {
    shell_t *sh = kzalloc(sizeof(*sh));
    if (!sh) return NULL;
    sh->con = con;
    sh->len = 0;

    /* Banner */
    fbcon_puts_inst(con, "\n"
               A_CYAN "  +----------------------------+" A_RESET "\n"
               A_CYAN "  |" A_WHITE "   EXO_OS  v0.1.0         " A_CYAN "|" A_RESET "\n"
               A_CYAN "  +----------------------------+" A_RESET "\n\n"
               "  Type " A_BOLD "help" A_RESET " to list commands.\n\n");

    fbcon_puts_inst(con, A_GREEN "exo" A_RESET "> ");
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
