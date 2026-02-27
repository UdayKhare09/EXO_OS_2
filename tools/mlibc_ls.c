#include <dirent.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *path = ".";
    if (argc > 1)
        path = argv[1];

    DIR *dir = opendir(path);
    if (!dir) {
        perror("ls");
        return 1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;
        puts(ent->d_name);
    }

    closedir(dir);
    return 0;
}
