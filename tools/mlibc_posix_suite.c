#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("[FAIL] %s (errno=%d)\n", msg, errno); \
            failures++; \
        } else { \
            printf("[PASS] %s\n", msg); \
        } \
    } while (0)

static int test_files(void) {
    const char *path = "/tmp/posix_suite_file.txt";
    const char *payload = "exo-posix-suite";
    char buf[64];

    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    CHECK(fd >= 0, "open/create file");
    if (fd < 0) return -1;

    ssize_t wr = write(fd, payload, strlen(payload));
    CHECK(wr == (ssize_t)strlen(payload), "write file");

    off_t off = lseek(fd, 0, SEEK_SET);
    CHECK(off == 0, "lseek rewind");

    memset(buf, 0, sizeof(buf));
    ssize_t rd = read(fd, buf, sizeof(buf) - 1);
    CHECK(rd == (ssize_t)strlen(payload), "read file");
    CHECK(strcmp(buf, payload) == 0, "verify file content");

    CHECK(close(fd) == 0, "close file");
    CHECK(unlink(path) == 0, "unlink file");
    return 0;
}

static int test_dirs(void) {
    char cwd[256];
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL, "getcwd");

    CHECK(mkdir("/tmp/posix_suite_dir", 0755) == 0, "mkdir");
    CHECK(chdir("/tmp/posix_suite_dir") == 0, "chdir into dir");

    char inner[256];
    CHECK(getcwd(inner, sizeof(inner)) != NULL, "getcwd after chdir");

    CHECK(chdir("/") == 0, "chdir back root");
    CHECK(rmdir("/tmp/posix_suite_dir") == 0, "rmdir");
    return 0;
}

static int test_readdir(void) {
    DIR *d = opendir("/bin");
    CHECK(d != NULL, "opendir /bin");
    if (!d) return -1;

    int entries = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, ".."))
            entries++;
    }

    CHECK(closedir(d) == 0, "closedir /bin");
    CHECK(entries >= 1, "readdir finds entries");
    return 0;
}

static int test_pipe_fork_wait(void) {
    int p[2];
    int prc = pipe(p);
    CHECK(prc == 0, "pipe");
    if (prc != 0) return -1;

    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }

    if (pid == 0) {
        const char *msg = "child-msg";
        close(p[0]);
        write(p[1], msg, strlen(msg));
        close(p[1]);
        _exit(0);
    }

    close(p[1]);

    struct pollfd fds = { .fd = p[0], .events = POLLIN, .revents = 0 };
    int pr = poll(&fds, 1, 2000);
    CHECK(pr == 1 && (fds.revents & POLLIN), "poll readable pipe");

    char buf[64] = {0};
    ssize_t rd = read(p[0], buf, sizeof(buf) - 1);
    CHECK(rd > 0, "read from child pipe");
    CHECK(strcmp(buf, "child-msg") == 0, "verify child pipe payload");

    close(p[0]);

    int st = 0;
    pid_t wp = waitpid(pid, &st, 0);
    CHECK(wp == pid, "waitpid child");
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child exit status");
    return 0;
}

static void *thread_main(void *arg) {
    (void)arg;
    return (void *)0x1234;
}

static int test_pthread(void) {
    pthread_t t;
    void *ret = NULL;

    CHECK(pthread_create(&t, NULL, thread_main, NULL) == 0, "pthread_create");
    CHECK(pthread_join(t, &ret) == 0, "pthread_join");
    CHECK(ret == (void *)0x1234, "pthread return value");
    return 0;
}

int main(void) {
    printf("mlibc posix suite start\n");

    test_files();
    test_dirs();
    test_readdir();
    test_pipe_fork_wait();
    test_pthread();

    if (failures) {
        printf("mlibc posix suite: FAIL (%d failures)\n", failures);
        return 1;
    }

    printf("mlibc posix suite: PASS\n");
    return 0;
}
