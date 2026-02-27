/* entry.S — EXO_OS user-space C runtime entry + raw syscall wrapper
 *
 * mlibc expects:
 *   _start(void)         — the ELF entry point
 *   __exo_syscall(nr, ...) — raw syscall, up to 6 args
 *
 * This is a C file using inline assembly so it can live in the same
 * build as the rest of the sysdeps (all C/C++).
 */

/* These are provided by mlibc at link time */
extern int main(int argc, char **argv, char **envp);
extern void __mlibc_entry(int (*main_fn)(int, char **, char **));

/* _start: ELF entry point
 *
 * The kernel sets up the user stack as:
 *   [rsp+0]  = argc
 *   [rsp+8]  = argv[0]  ... argv[argc-1], NULL
 *   then envp[0] ... envp[n], NULL
 *   then auxv entries
 *
 * mlibc's __mlibc_entry expects a function pointer to main; it will
 * parse argc/argv/envp/auxv from the stack itself.
 */
__attribute__((naked, noreturn))
void _start(void) {
    __asm__ volatile (
        /* Zero the frame pointer (ABI-required for backtraces) */
        "xorq %%rbp, %%rbp\n\t"
        /* Call __mlibc_entry(&main) — mlibc's C startup */
        "leaq main(%%rip), %%rdi\n\t"
        "call __mlibc_entry\n\t"
        /* Should not return, but just in case */
        "movq $60, %%rax\n\t"    /* SYS_EXIT */
        "xorq %%rdi, %%rdi\n\t"
        "syscall\n\t"
        "ud2\n\t"
        ::: "memory"
    );
}

/* __exo_syscall — raw SYSCALL wrapper
 *
 * Prototype: long __exo_syscall(long nr, long a1, long a2, long a3,
 *                                long a4, long a5, long a6);
 *
 * Linux x86-64 SYSCALL convention:
 *   nr   → rax
 *   arg1 → rdi   (already there from C ABI)
 *   arg2 → rsi   (already there)
 *   arg3 → rdx   (already there)
 *   arg4 → r10   (C ABI uses rcx, so move it)
 *   arg5 → r8    (already there)
 *   arg6 → r9    (already there)
 *   return → rax
 *   rcx and r11 are clobbered by SYSCALL instruction
 */
long __exo_syscall(long nr, long a1, long a2, long a3,
                   long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}
