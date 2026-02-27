/* tools/hello.c — Minimal freestanding Hello World for EXO_OS user-space
 *
 * Compiled with: clang --target=x86_64-unknown-linux-elf -nostdlib -nostdinc
 *                      -static -ffreestanding -o build/hello tools/hello.c
 *
 * Uses raw SYSCALL instruction — no libc, no runtime.
 */

static long exo_syscall3(long nr, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static long exo_syscall1(long nr, long a1) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

__attribute__((force_align_arg_pointer))
void _start(void) {
    /* SYS_WRITE = 1, fd = 1 (stdout), msg, len */
    static const char msg[] = "Hello World from EXO_OS userspace!\n";
    exo_syscall3(1, 1, (long)msg, sizeof(msg) - 1);

    /* SYS_EXIT = 60, status = 0 */
    exo_syscall1(60, 0);

    /* Should never reach here */
    for (;;) __asm__ volatile("hlt");
}
