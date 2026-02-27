# EXO_OS mlibc Port Plan

## Scope
This plan targets a practical, incremental Linux-ABI-compatible port path for `mlibc` on EXO_OS (x86_64). It is based on the current kernel surface in this repo.

## Current Status (What You Already Have)

### Core syscall plumbing
- INT `0x80` + fast `SYSCALL` entry/dispatch are implemented.
- Sparse syscall table and Linux x86_64 numbering are in place.
- Files: `src/kernel/syscall/syscall.c`, `src/kernel/sched/context_switch.asm`, `src/kernel/syscall/syscall.h`.

### Process / memory / threading primitives
- Implemented: `fork`, `clone` (subset), `execve`, `wait4`, `exit`, `exit_group`.
- Implemented: `mmap`/`munmap`/`mprotect`/`brk` (anonymous mappings, basic behavior).
- Implemented: `futex` (`WAIT`/`WAKE` subset), `set_tid_address`, `arch_prctl` (`FS/GS`).
- Files: `src/kernel/syscall/proc_syscalls.c`, `src/kernel/syscall/syscall.c`.

### Filesystem + fd basics
- Implemented: `open`, `close`, `read`, `write`, `lseek`, `stat/fstat/lstat`, `fstatat` (simplified), `getdents64`.
- Implemented: `dup/dup2/dup3`, `pipe/pipe2`, `fcntl` subset.
- Implemented path operations: `getcwd/chdir/mkdir/rmdir/unlink/rename/readlink/access`.
- Files: `src/kernel/syscall/file_syscalls.c`, `src/kernel/fs/vfs.c`, `src/kernel/fs/fd.c`, `src/kernel/ipc/pipe.c`.

### Signal basics
- Implemented: `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`, pending-mask delivery.
- Files: `src/kernel/ipc/signal.c`, `src/kernel/syscall/syscall.c`.

### Networking basics
- Implemented: `socket/connect/accept/sendto/recvfrom/shutdown/bind/listen/getsockname/getpeername/setsockopt/getsockopt`.
- File: `src/kernel/syscall/net_syscalls.c`.

## Primary Gaps for a Full mlibc Port

## 1. `*at` syscall family is missing/incomplete (high priority)
Why it matters:
- Modern libc code paths prefer dirfd-based APIs (`openat`, `mkdirat`, `unlinkat`, `renameat`, `readlinkat`, etc.).

Current state:
- `fstatat` exists but ignores `dirfd` and partially ignores flags.
- No `openat`, `linkat`, `symlinkat`, `renameat(2)`, `renameat2`, `mkdirat`, `unlinkat`, `faccessat`.

Add:
- `openat`, `mkdirat`, `unlinkat`, `renameat`, `readlinkat`, `faccessat`, `linkat`, `symlinkat`.
- Proper `AT_FDCWD` + relative path resolution against directory fds.

Touchpoints:
- `src/kernel/syscall/syscall.h`
- `src/kernel/syscall/syscall.c`
- `src/kernel/syscall/file_syscalls.c`
- `src/kernel/fs/vfs.c`

## 2. Errno contract is inconsistent (high priority)
Why it matters:
- mlibc expects Linux-style negative errno returns from kernel.

Current issue:
- `net_syscalls.c` often returns plain `-1` instead of `-E*`.

Fix:
- Normalize all syscall exits to `-errno`.
- Audit all syscall files for raw `-1`, `0/1` ad-hoc error signaling.

Touchpoints:
- `src/kernel/syscall/net_syscalls.c`
- `src/kernel/syscall/file_syscalls.c`
- `src/kernel/syscall/proc_syscalls.c`
- `src/kernel/syscall/syscall.c`

## 3. Threading ABI completeness (high priority)
Why it matters:
- pthread on mlibc needs robust thread teardown and synchronization contracts.

Current gaps:
- No `gettid`, `tgkill`, `set_robust_list`, `get_robust_list`.
- `futex` supports only WAIT/WAKE (no requeue/private variants/timeouts robustly).
- `clone` semantics are simplified.

Add:
- `gettid`, `tgkill` at minimum.
- `set_robust_list` (needed by many thread runtimes).
- Expand futex ops incrementally (`FUTEX_WAIT_BITSET`, `FUTEX_WAKE_BITSET`, optionally `REQUEUE`).

Touchpoints:
- `src/kernel/syscall/syscall.h`
- `src/kernel/syscall/syscall.c`
- `src/kernel/syscall/proc_syscalls.c`
- `src/kernel/sched/*`

## 4. Time + scheduling syscall surface (medium priority)
Why it matters:
- libc and apps use sleep/time APIs heavily.

Current state:
- `clock_gettime` exists.
- Missing likely-needed: `nanosleep`, `clock_nanosleep`, `gettimeofday`, `times`, `sched_yield`.

Add in order:
- `nanosleep`
- `sched_yield`
- `gettimeofday`
- optional: `clock_nanosleep`

Touchpoints:
- `src/kernel/syscall/syscall.h`
- `src/kernel/syscall/syscall.c`
- timer/sched modules.

## 5. `ioctl` + TTY/termios behavior (high priority for interactive userspace)
Why it matters:
- shell, line editing, and many libc/stdin paths depend on termios ioctls.

Current state:
- `ioctl` dispatcher exists, but coverage is minimal.

Add:
- Minimal termios ioctls (`TCGETS`, `TCSETS`, window size if needed).
- FIONBIO + basic fd control ioctls.

Touchpoints:
- `src/kernel/syscall/net_syscalls.c` (`sys_ioctl` path)
- relevant tty/input/console drivers.

## 6. Polling/event APIs are minimal (medium priority)
Why it matters:
- mlibc and real applications often depend on robust `poll` semantics or epoll.

Current state:
- `poll` is “check once + simplified sleep/retry”.

Improve:
- Correct timeout semantics.
- Wakeup integration for pipes/sockets/tty.
- Optional next step: `ppoll`, `epoll_*`.

Touchpoints:
- `src/kernel/syscall/net_syscalls.c`
- `src/kernel/ipc/pipe.c`
- socket/file ops poll handlers.

## 7. `mmap` completeness (medium priority)
Why it matters:
- Dynamic linking and richer libc use cases need file-backed mappings and proper flags behavior.

Current state:
- Anonymous mappings only (`MAP_ANONYMOUS` required).

Add:
- File-backed `mmap` for at least `MAP_PRIVATE` + read-only executable mappings.
- Proper handling for `MAP_FIXED`, `MAP_SHARED` semantics (phased).

Touchpoints:
- `src/kernel/syscall/proc_syscalls.c`
- VFS/read paths.

## 8. Path/mount/symlink semantics (medium priority)
Why it matters:
- libc tests exercise corner cases of symlink traversal and `..` crossing mount points.

Current state:
- TODO exists for proper `..` across mountpoints.
- Symlink handling is simplified.

Add:
- Correct parent traversal across mount boundaries.
- Tighten symlink behavior for Linux-compatible edge cases.

Touchpoints:
- `src/kernel/fs/vfs.c`

## 9. Filesystem behavior gaps that will surface in libc tests (medium priority)
Current known item:
- ext2 unlink path has TODO for indirect block reclamation.

Add:
- Finish ext2 truncate/unlink block reclamation paths.
- Improve metadata consistency and edge handling.

Touchpoints:
- `src/kernel/fs/ext2/ext2.c`

## 10. Optional but very useful libc-oriented syscalls (later)
- `uname`
- `getrandom`
- `prlimit64` / `getrlimit`
- `statx` (optional; many stacks can fall back)
- `readv` (you already have `writev`)
- `pread64` / `pwrite64`

## Phased Implementation Plan

## Phase 0: ABI correctness pass (do first)
1. Normalize errno returns to strict `-E*` everywhere.
2. Validate syscall arg handling and pointer checks for consistency.
3. Add a syscall conformance test for negative errno mapping.

## Phase 1: `*at` and path-fd model
1. Implement `openat` + `AT_FDCWD`.
2. Convert existing path syscalls to internal `*at` helpers.
3. Implement `mkdirat`, `unlinkat`, `renameat`, `readlinkat`, `faccessat`.
4. Make `fstatat` respect `dirfd` and flags fully.

## Phase 2: pthread-critical primitives
1. Add `gettid`, `tgkill`.
2. Add `set_robust_list` + kernel bookkeeping.
3. Extend futex behavior (timeouts + additional ops).
4. Tighten `clone` behavior for thread-group semantics.

## Phase 3: TTY/ioctl + poll correctness
1. Implement core termios ioctls.
2. Fix poll timeout/wakeup semantics.
3. Add pipe/socket poll integration tests.

## Phase 4: mmap + file mapping
1. Implement file-backed `mmap` (`MAP_PRIVATE`, read-only first).
2. Add `msync`/shared semantics later if needed.
3. Validate with dynamic-loader-style mapping tests.

## Phase 5: libc convenience syscalls
1. `uname`, `getrandom`, `nanosleep`, `sched_yield`, `gettimeofday`.
2. `pread64/pwrite64`, `readv`.
3. `prlimit64/getrlimit` minimal implementation.

## Phase 6: hardening and compatibility
1. Address VFS mount/symlink corner cases.
2. Improve ext2 reclaim/truncate correctness.
3. Run full mlibc + app test matrix, fix regressions.

## Recommended Immediate Additions (highest ROI)
1. `openat` + proper `fstatat(dirfd,...)`
2. `gettid`, `tgkill`, `set_robust_list`
3. errno normalization in `net_syscalls.c`
4. termios ioctl minimum set
5. `nanosleep` + `sched_yield`

## Validation Strategy

## Kernel-side
1. Extend `tools/syscall_test.c` with:
- `openat`/`fstatat(dirfd)`
- thread/futex/clone edge cases
- `nanosleep` timing checks
- poll timeout correctness

## libc-side
1. Bring up mlibc with static hello + pthread smoke test.
2. Run mlibc POSIX subset tests by phase.
3. Track failures by syscall and close gaps iteratively.

## Definition of Done for “usable mlibc port”
- mlibc startup + dynamic loader path work.
- pthread create/join/mutex/condvar stable.
- shell utilities (`ls`, `cat`, `mkdir`, `rm`, `mv`, `sh`) run without ABI workarounds.
- Networked user app (simple TCP client) runs with correct errno and poll behavior.

