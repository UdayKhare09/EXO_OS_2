#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int rm_path(const char *path, int recursive, int force) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (force && errno == ENOENT)
            return 0;
        fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            fprintf(stderr, "rm: %s: is a directory\n", path);
            return 1;
        }

        DIR *d = opendir(path);
        if (!d) {
            fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
            return 1;
        }

        int rc = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
                continue;
            char child[512];
            int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            if (n <= 0 || (size_t)n >= sizeof(child)) {
                rc = 1;
                continue;
            }
            rc |= rm_path(child, recursive, force);
        }
        closedir(d);

        if (rmdir(path) < 0) {
            fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
            rc = 1;
        }
        return rc;
    }

    if (unlink(path) < 0) {
        if (force && errno == ENOENT)
            return 0;
        fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int recursive = 0;
    int force = 0;
    int first = 1;

    while (first < argc && argv[first][0] == '-') {
        const char *opt = argv[first] + 1;
        if (!*opt)
            break;
        while (*opt) {
            if (*opt == 'r' || *opt == 'R')
                recursive = 1;
            else if (*opt == 'f')
                force = 1;
            else {
                fprintf(stderr, "rm: unknown option -%c\n", *opt);
                return 1;
            }
            opt++;
        }
        first++;
    }

    if (first >= argc) {
        fprintf(stderr, "usage: rm [-rf] FILE...\n");
        return 1;
    }

    int rc = 0;
    for (int i = first; i < argc; i++)
        rc |= rm_path(argv[i], recursive, force);
    return rc;
}
