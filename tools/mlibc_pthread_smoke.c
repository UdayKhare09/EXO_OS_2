#include <pthread.h>
#include <stdio.h>

static void *thread_main(void *arg) {
    (void)arg;
    return (void *)0x1234;
}

int main(void) {
    pthread_t t;
    void *retval = NULL;

    if (pthread_create(&t, NULL, thread_main, NULL) != 0) return 1;
    if (pthread_join(t, &retval) != 0) return 2;

    printf("pthread smoke retval=%p\n", retval);
    return retval == (void *)0x1234 ? 0 : 3;
}
