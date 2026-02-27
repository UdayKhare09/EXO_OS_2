#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/mman.h>

#include <bits/ensure.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/thread.hpp>

#define CLONE_VM             0x00000100UL
#define CLONE_FS             0x00000200UL
#define CLONE_FILES          0x00000400UL
#define CLONE_SIGHAND        0x00000800UL
#define CLONE_THREAD         0x00010000UL
#define CLONE_SETTLS         0x00080000UL
#define CLONE_PARENT_SETTID  0x00100000UL

extern "C" long __exo_spawn_thread(unsigned long flags, void *stack,
        int *parent_tid_ptr, void *tls);

extern "C" void __mlibc_enter_thread(void *entry, void *user_arg) {
    auto tcb = mlibc::get_current_tcb();

    while(!__atomic_load_n(&tcb->tid, __ATOMIC_RELAXED))
        mlibc::sys_futex_wait(&tcb->tid, 0, nullptr);

    tcb->invokeThreadFunc(entry, user_arg);

    __atomic_store_n(&tcb->didExit, 1, __ATOMIC_RELEASE);
    mlibc::sys_futex_wake(&tcb->didExit, true);

    mlibc::sys_thread_exit();
}

namespace mlibc {

static constexpr size_t default_stacksize = 0x200000;

int sys_prepare_stack(void **stack, void *entry, void *user_arg, void *tcb,
        size_t *stack_size, size_t *guard_size, void **stack_base) {
    (void)tcb;

    if (!*stack_size)
        *stack_size = default_stacksize;

    uintptr_t map;
    if (*stack) {
        map = reinterpret_cast<uintptr_t>(*stack);
        *guard_size = 0;
    } else {
        map = reinterpret_cast<uintptr_t>(mmap(nullptr,
                *stack_size + *guard_size,
                PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS,
                -1,
                0));
        if (reinterpret_cast<void *>(map) == MAP_FAILED)
            return EAGAIN;

        int ret = mprotect(reinterpret_cast<void *>(map + *guard_size),
                *stack_size,
                PROT_READ | PROT_WRITE);
        if (ret)
            return EAGAIN;
    }

    *stack_base = reinterpret_cast<void *>(map);

    auto sp = reinterpret_cast<uintptr_t *>(map + *guard_size + *stack_size);
    *--sp = reinterpret_cast<uintptr_t>(user_arg);
    *--sp = reinterpret_cast<uintptr_t>(entry);
    *stack = reinterpret_cast<void *>(sp);

    return 0;
}

int sys_clone(void *tcb, pid_t *pid_out, void *stack) {
    unsigned long flags = CLONE_VM | CLONE_FS | CLONE_FILES |
            CLONE_SIGHAND | CLONE_THREAD | CLONE_SETTLS |
            CLONE_PARENT_SETTID;

    long ret = __exo_spawn_thread(flags, stack, (int *)pid_out, tcb);
    if (ret < 0)
        return (int)(-ret);

    return 0;
}

[[noreturn]] void sys_thread_exit() {
    sys_exit(0);
    __builtin_unreachable();
}

}
