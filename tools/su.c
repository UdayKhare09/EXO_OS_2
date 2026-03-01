#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <crypt.h>
#include <grp.h>

#define PASSWD_FILE "/etc/passwd"
#define SHADOW_FILE "/etc/shadow"

typedef struct {
    char username[64];
    uid_t uid;
    gid_t gid;
    char home[256];
    char shell[256];
} user_info_t;

static int lookup_user(const char *name, user_info_t *out) {
    if (!name || !name[0] || !out) return -1;
    FILE *f = fopen(PASSWD_FILE, "r");
    if (!f) return -1;

    char line[512];
    int rc = -1;
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
        rc = 0;
        break;
    }

    fclose(f);
    return rc;
}

static int read_shadow_hash(const char *name, char *out, size_t out_sz) {
    if (!name || !name[0] || !out || out_sz == 0) return -1;

    FILE *f = fopen(SHADOW_FILE, "r");
    if (!f) return -1;

    char line[768];
    int rc = -1;
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
        rc = 0;
        break;
    }

    fclose(f);
    return rc;
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

    fputs("Password: ", stdout);
    fflush(stdout);
    if (!fgets(out, (int)out_sz, stdin))
        return -1;

    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';
    return 0;
}

static int verify_password_for_user(const char *name) {
    char hash[256];
    char pw[128];
    if (read_shadow_hash(name, hash, sizeof(hash)) < 0)
        return -1;

    if (hash[0] == '!' || hash[0] == '*')
        return -1;

    if (read_password(pw, sizeof(pw)) < 0) return -1;

    if (hash[0] == '\0')
        return pw[0] == '\0' ? 0 : -1;

    char *calc = crypt(pw, hash);
    if (!calc) return -1;
    return strcmp(calc, hash) == 0 ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *target = (argc > 1) ? argv[1] : "root";

    user_info_t user;
    if (lookup_user(target, &user) < 0) {
        fprintf(stderr, "su: unknown user '%s'\n", target);
        return 1;
    }
    if (shell_login_disabled(user.shell)) {
        fprintf(stderr, "su: account '%s' is not available\n", target);
        return 1;
    }

    if (getuid() != 0) {
        if (verify_password_for_user(target) < 0) {
            fprintf(stderr, "su: authentication failure\n");
            return 1;
        }
    }

    gid_t groups[1] = { user.gid };
    if (setgroups(1, groups) < 0) {
        perror("su: setgroups");
        return 1;
    }
    if (setgid(user.gid) < 0) {
        perror("su: setgid");
        return 1;
    }
    if (setuid(user.uid) < 0) {
        perror("su: setuid");
        return 1;
    }

    if (user.home[0]) chdir(user.home);
    setenv("HOME", user.home[0] ? user.home : "/", 1);
    setenv("USER", user.username, 1);
    setenv("LOGNAME", user.username, 1);
    setenv("SHELL", user.shell[0] ? user.shell : "/bin/sh", 1);

    char *const sh_argv[] = { (char *)"-sh", NULL };
    execve(user.shell[0] ? user.shell : "/bin/sh", sh_argv, environ);

    perror("su: execve");
    return 1;
}
