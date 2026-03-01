/*
 * init.c — PID 1 for EXO_OS
 *
 * Linux-compatible behaviour:
 *  - Open /dev/console as stdin/stdout/stderr
 *  - SIG_IGN SIGINT, SIGTERM, SIGHUP (PID 1 ignores these by default)
 *  - Create a new session (setsid) so the console becomes a controlling TTY
 *  - fork → child does setsid + open /dev/console again + TIOCSCTTY + execve shell
 *  - Parent loops: waitpid(-1, ...) reaping all zombies; if the shell
 *    child died, respawn it after 1 s.
 *  - Load /etc/environment once before spawning the shell.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <crypt.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <grp.h>

/* ── Constants ────────────────────────────────────────────────────────────── */
#define CONSOLE       "/dev/console"
#define SHELL_PATH    "/bin/sh"
#define ENV_FILE      "/etc/environment"
#define PASSWD_FILE   "/etc/passwd"
#define SHADOW_FILE   "/etc/shadow"
#define RESPAWN_DELAY 1            /* seconds between shell restarts           */
#define MAX_ENV       256          /* maximum number of env vars to carry      */

/* POSIX TIOCSCTTY — same number as Linux x86-64 */
#ifndef TIOCSCTTY
#define TIOCSCTTY 0x540E
#endif

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Reopen fd onto /dev/console.  Needed because we start with no open fds. */
static void open_console_fd(int wanted_fd, int oflags) {
    int fd = open(CONSOLE, oflags | O_NOCTTY);
    if (fd < 0) {
        /* Fallback: /dev/tty or /dev/null */
        fd = open("/dev/tty", oflags | O_NOCTTY);
        if (fd < 0) fd = open("/dev/null", oflags | O_NOCTTY);
        if (fd < 0) return;
    }
    if (fd != wanted_fd) {
        dup2(fd, wanted_fd);
        close(fd);
    }
}

/* Read /etc/environment (KEY=VALUE lines) into the environment. */
static void load_environment(void) {
    FILE *f = fopen(ENV_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r'))
            line[--l] = '\0';
        if (l == 0 || line[0] == '#') continue;
        /* Must have '=' to be a valid KEY=VALUE pair */
        if (!strchr(line, '=')) continue;
        putenv(strdup(line));
    }
    fclose(f);
}

/* ── Shell spawner ────────────────────────────────────────────────────────── */

typedef struct {
    char username[64];
    uid_t uid;
    gid_t gid;
    char home[256];
    char shell[256];
} login_user_t;

static int parse_passwd_user(const char *name, login_user_t *out) {
    if (!name || !name[0] || !out) return -1;

    FILE *f = fopen(PASSWD_FILE, "r");
    if (!f) return -1;

    char line[512];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;

        char *save = NULL;
        char *u = strtok_r(line, ":", &save);
        char *pw = strtok_r(NULL, ":", &save);
        char *uid_s = strtok_r(NULL, ":", &save);
        char *gid_s = strtok_r(NULL, ":", &save);
        char *gecos = strtok_r(NULL, ":", &save);
        char *home = strtok_r(NULL, ":", &save);
        char *shell = strtok_r(NULL, ":", &save);
        (void)pw; (void)gecos;

        if (!u || !uid_s || !gid_s || !home || !shell) continue;
        if (strcmp(u, name) != 0) continue;

        memset(out, 0, sizeof(*out));
        strncpy(out->username, u, sizeof(out->username) - 1);
        out->uid = (uid_t)strtoul(uid_s, NULL, 10);
        out->gid = (gid_t)strtoul(gid_s, NULL, 10);
        strncpy(out->home, home, sizeof(out->home) - 1);
        strncpy(out->shell, shell, sizeof(out->shell) - 1);
        found = 0;
        break;
    }

    fclose(f);
    return found;
}

static int read_shadow_hash(const char *name, char *out, size_t out_sz) {
    if (!name || !name[0] || !out || out_sz == 0) return -1;

    FILE *f = fopen(SHADOW_FILE, "r");
    if (!f) return -1;

    char line[768];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;

        char *save = NULL;
        char *u = strtok_r(line, ":", &save);
        char *hash = strtok_r(NULL, ":", &save);
        if (!u || !hash) continue;
        if (strcmp(u, name) != 0) continue;

        strncpy(out, hash, out_sz - 1);
        out[out_sz - 1] = '\0';
        found = 0;
        break;
    }

    fclose(f);
    return found;
}

static int shell_login_disabled(const char *shell) {
    if (!shell || !shell[0]) return 0;
    return strcmp(shell, "/sbin/nologin") == 0 ||
           strcmp(shell, "/usr/sbin/nologin") == 0 ||
           strcmp(shell, "/bin/false") == 0;
}

static int read_password(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return -1;
    out[0] = '\0';

    write(STDOUT_FILENO, "\nPassword: ", 10);
    if (!fgets(out, (int)out_sz, stdin))
        return -1;

    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';
    return 0;
}

static int verify_user_password(const char *username) {
    char hash[256];
    char pw[128];
    if (read_shadow_hash(username, hash, sizeof(hash)) < 0)
        return -1;

    /* '!' or '*' means account is locked. */
    if (hash[0] == '!' || hash[0] == '*')
        return -1;

    if (read_password(pw, sizeof(pw)) < 0) return -1;

    if (hash[0] == '\0')
        return pw[0] == '\0' ? 0 : -1;

    char *calc = crypt(pw, hash);
    if (!calc) return -1;
    return strcmp(calc, hash) == 0 ? 0 : -1;
}

static int prompt_login(login_user_t *out) {
    char username[64];

    for (;;) {
        write(STDOUT_FILENO, "\nexo login: ", 12);
        if (!fgets(username, sizeof(username), stdin)) {
            strcpy(username, "root");
        }

        size_t len = strlen(username);
        while (len > 0 && (username[len - 1] == '\n' || username[len - 1] == '\r'))
            username[--len] = '\0';
        if (len == 0) strcpy(username, "root");

        if (parse_passwd_user(username, out) == 0 &&
            !shell_login_disabled(out->shell) &&
            verify_user_password(username) == 0)
            return 0;

        write(STDOUT_FILENO, "Login incorrect\n", 16);
    }
}

static pid_t spawn_shell(const login_user_t *user) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("init: fork");
        return -1;
    }
    if (pid == 0) {
        /* ── Child: set up its own session and controlling terminal ───── */

        /* New session: child becomes session leader, loses any ctty */
        if (setsid() < 0) {
            perror("init: setsid");
            _exit(1);
        }

        /* Make console the controlling terminal, but route stdio via /dev/tty.
         * /dev/console is 0600 root; keeping child stdio on it breaks non-root
         * shells because this kernel re-checks vnode permissions on each I/O. */
        int ctty_fd = open(CONSOLE, O_RDWR | O_NOCTTY);
        if (ctty_fd < 0) ctty_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (ctty_fd >= 0)
            ioctl(ctty_fd, TIOCSCTTY, 0);

        int io_fd = open("/dev/tty", O_RDWR);
        if (io_fd < 0) io_fd = ctty_fd;
        if (io_fd >= 0) {
            dup2(io_fd, STDIN_FILENO);
            dup2(io_fd, STDOUT_FILENO);
            dup2(io_fd, STDERR_FILENO);
        }
        if (ctty_fd >= 0 && ctty_fd != io_fd && ctty_fd > 2) close(ctty_fd);
        if (io_fd > 2) close(io_fd);

        /* Restore default signal handlers for child */
        signal(SIGINT,  SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        if (user) {
            gid_t groups[1] = { user->gid };
            if (setgroups(1, groups) < 0) {
                perror("init: setgroups");
                _exit(1);
            }
            if (setgid(user->gid) < 0) {
                perror("init: setgid");
                _exit(1);
            }
            if (setuid(user->uid) < 0) {
                perror("init: setuid");
                _exit(1);
            }

            if (user->home[0]) chdir(user->home);

            setenv("HOME", user->home[0] ? user->home : "/", 1);
            setenv("USER", user->username, 1);
            setenv("LOGNAME", user->username, 1);
            setenv("SHELL", user->shell[0] ? user->shell : SHELL_PATH, 1);
        }

        /* Login shell so BusyBox ash sources /etc/profile. */
        const char *shell = (user && user->shell[0]) ? user->shell : SHELL_PATH;
        char *const argv[] = { (char *)"-sh", NULL };
        execve(shell, argv, environ);

        /* execve failed: try /bin/busybox sh */
        char *const bav[] = { (char *)"busybox", (char *)"sh", (char *)"-l", NULL };
        execve("/bin/busybox", bav, environ);

        perror("init: exec shell");
        _exit(127);
    }
    return pid;   /* parent: return child PID */
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    /* ── 1. Set up stdio on /dev/console ─────────────────────────────────── */
    open_console_fd(STDIN_FILENO,  O_RDONLY);
    open_console_fd(STDOUT_FILENO, O_WRONLY);
    open_console_fd(STDERR_FILENO, O_WRONLY);

    /* Announce ourselves */
    write(STDOUT_FILENO, "\r\nEXO_OS init starting...\r\n", 27);

    /* ── 2. PID 1 signal disposition ────────────────────────────────────── */
    /* SIGINT/SIGTERM/SIGHUP are ignored by init; kernel never delivers them
     * unless explicitly sent.  SIGCHLD must be SIG_DFL so waitpid works. */
    signal(SIGINT,  SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);   /* stop from keyboard: init ignores */
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);   /* default: let waitpid collect status   */

    /* ── 3. Become session leader ────────────────────────────────────────── */
    setsid();   /* should be a no-op (already PID 1 == sid 1), but be safe */

    /* ── 4. Load environment from /etc/environment ───────────────────────── */
    load_environment();

    /* Set reasonable defaults if missing */
    if (!getenv("HOME"))  putenv("HOME=/home/root");
    if (!getenv("PATH"))  putenv("PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin");
    if (!getenv("TERM"))  putenv("TERM=linux");
    if (!getenv("SHELL")) putenv("SHELL=" SHELL_PATH);
    

    /* ── 5. Spawn login shell and supervise it ───────────────────────────── */
    login_user_t current_user;
    if (prompt_login(&current_user) < 0) {
        memset(&current_user, 0, sizeof(current_user));
        strcpy(current_user.username, "root");
        current_user.uid = 0;
        current_user.gid = 0;
        strcpy(current_user.home, "/home/root");
        strcpy(current_user.shell, SHELL_PATH);
    }

    pid_t shell_pid = spawn_shell(&current_user);

    for (;;) {
        int status = 0;
        /* Reap ALL children (not just shell); this harvests zombie processes
         * that were reparented to init when their original parent exited. */
        pid_t reaped = waitpid(-1, &status, 0);  /* blocking wait */
        if (reaped < 0) {
            if (errno == EINTR) continue;     /* interrupted by signal */
            if (errno == ECHILD) {
                /* No children at all — respawn shell */
                sleep(RESPAWN_DELAY);
                prompt_login(&current_user);
                shell_pid = spawn_shell(&current_user);
                continue;
            }
            /* Unexpected error */
            sleep(1);
            continue;
        }

        if (reaped == shell_pid) {
            /* Our main shell child died — respawn after a short delay */
            if (WIFEXITED(status))
                fprintf(stderr, "init: shell exited with status %d\n",
                        WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                fprintf(stderr, "init: shell killed by signal %d\n",
                        WTERMSIG(status));
            sleep(RESPAWN_DELAY);
            prompt_login(&current_user);
            shell_pid = spawn_shell(&current_user);
        }
        /* else: some other child was reaped (zombie cleanup) — just continue */
    }

    /* unreachable */
    return 0;
}
