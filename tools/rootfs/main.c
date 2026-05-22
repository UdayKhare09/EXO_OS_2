#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

int main() {
    printf("Hello, EXO OS!\n");
    pid_t pid = fork();
    if (pid == 0) {
        printf("Child: PID=%d, PPID=%d\n", getpid(), getppid());
    } else {
        printf("Parent: PID=%d, Child PID=%d\n", getpid(), pid);
        wait(NULL);
    }
    return 0;
}