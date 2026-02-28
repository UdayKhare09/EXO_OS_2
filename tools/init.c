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
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

/* ── Constants ────────────────────────────────────────────────────────────── */
#define CONSOLE       "/dev/console"
#define SHELL_PATH    "/bin/sh"
#define ENV_FILE      "/etc/environment"
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

static pid_t spawn_shell(void) {
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

        /* Open /dev/console and make it the controlling terminal */
        int fd = open(CONSOLE, O_RDWR);
        if (fd < 0) fd = open("/dev/tty", O_RDWR);
        if (fd >= 0) {
            /* TIOCSCTTY 0 = steal if already owned; harmless if not */
            ioctl(fd, TIOCSCTTY, 0);

            /* Move to stdin/stdout/stderr */
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > 2) close(fd);
        }

        /* Restore default signal handlers for child */
        signal(SIGINT,  SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        /* Login shell so BusyBox ash sources /etc/profile. */
        char *const argv[] = { (char *)"-sh", NULL };
        execve(SHELL_PATH, argv, environ);

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
    if (!getenv("HOME"))  putenv("HOME=/root");
    if (!getenv("PATH"))  putenv("PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin");
    if (!getenv("TERM"))  putenv("TERM=linux");
    if (!getenv("SHELL")) putenv("SHELL=" SHELL_PATH);
    

    /* ── 5. Spawn the first shell and supervise it ───────────────────────── */
    pid_t shell_pid = spawn_shell();

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
                shell_pid = spawn_shell();
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
            shell_pid = spawn_shell();
        }
        /* else: some other child was reaped (zombie cleanup) — just continue */
    }

    /* unreachable */
    return 0;
}
