## Plan: Linux-like Users, Groups & Permissions for EXO_OS

**TL;DR** — EXO_OS already has the skeleton (uid/gid on tasks and vnodes, basic DAC checks, passwd/group files) but is missing the Linux credential model (real/effective/saved/fs IDs), setuid-on-exec, capabilities, signal/IPC permission enforcement, special-bit handling, and a userspace login flow. This plan adds all of that in a single sweep, organized by subsystem so dependencies are clear. Every kernel task will carry a `struct cred` instead of bare uid/gid fields; the VFS will handle all 12 permission bits; the ELF loader will honor setuid/setgid; a basic capabilities framework will gate privileged operations; and init will parse `/etc/passwd` and drop to a user shell.

---

**Steps**

### A. Introduce `struct cred` — Credential Abstraction

1. Create a new header [src/kernel/sched/cred.h](src/kernel/sched/cred.h) defining:
   - `struct cred` with fields: `uid`, `euid`, `suid`, `fsuid`, `gid`, `egid`, `sgid`, `fsgid`, `uint64_t cap_effective`, `cap_permitted`, `cap_inheritable`, `cap_bounding`, `uint32_t securebits`, `uint32_t groups[TASK_MAX_GROUPS]`, `uint32_t group_count`.
   - Raise `TASK_MAX_GROUPS` to at least 32 (define in this header).
   - Helper inlines: `cred_init(struct cred *c)`, `cred_copy(dst, src)`, `capable(cred, cap)` (checks `cap_effective` bit), `uid_eq()`, `gid_eq()`.

2. In [src/kernel/sched/task.h](src/kernel/sched/task.h), replace the bare `uid`, `gid`, `umask`, `groups[]`, `group_count` fields in `task_t` with a single `struct cred cred` member (plus keep `umask` separate — it's not a credential).

3. Update [src/kernel/sched/task.c](src/kernel/sched/task.c) `task_create()` (~line 120) to call `cred_init()` which sets all IDs to 0, grants full `cap_*` to uid-0 tasks, and sets `securebits = 0`.

4. Update fork/clone in [src/kernel/syscall/proc_syscalls.c](src/kernel/syscall/proc_syscalls.c#L248-L259) to do `cred_copy(&child->cred, &parent->cred)` instead of field-by-field copy.

5. **Global search-and-replace** every `cur->uid` / `cur->gid` / `cur->groups` / `cur->group_count` access across the kernel to go through `cur->cred.euid` / `cur->cred.egid` (or the appropriate ID variant depending on context — filesystem ops use `fsuid`/`fsgid`, signal checks use `euid`/`egid`, etc.). Key files:
   - [src/kernel/syscall/syscall.c](src/kernel/syscall/syscall.c) — all `sc_getuid`, `sc_setuid`, `sc_getgid`, `sc_setgid`, `sc_getgroups`, `sc_setgroups`, `sc_geteuid`, `sc_getegid`
   - [src/kernel/syscall/file_syscalls.c](src/kernel/syscall/file_syscalls.c) — `check_vnode_access()` and all callers
   - [src/kernel/syscall/proc_syscalls.c](src/kernel/syscall/proc_syscalls.c) — execve auxvec push
   - [src/kernel/syscall/net_syscalls.c](src/kernel/syscall/net_syscalls.c) — ioctl uid/gid reporting
   - [src/kernel/fs/procfs.c](src/kernel/fs/procfs.c) — `/proc/<pid>/status` Uid/Gid/Cap lines
   - [src/kernel/net/unix.c](src/kernel/net/unix.c) — peer credential recording
   - [src/kernel/ipc/signal.c](src/kernel/ipc/signal.c) — (will need cred for permission checks, see step F)

### B. Implement Full Credential Syscalls

6. In [src/kernel/syscall/syscall.c](src/kernel/syscall/syscall.c), update existing syscalls:
   - `sc_getuid` (102) → return `cred.uid` (real uid)
   - `sc_geteuid` (107) → return `cred.euid`
   - `sc_getgid` (104) → return `cred.gid` (real gid)
   - `sc_getegid` (108) → return `cred.egid`
   - `sc_setuid` (105) → Linux semantics: if `capable(CAP_SETUID)`, set all of `uid/euid/suid/fsuid`; else only set `euid` if arg matches `uid` or `suid`
   - `sc_setgid` (106) → analogous logic with `CAP_SETGID`

7. Add **new syscalls** (register in the syscall dispatch table):
   - `setreuid` (113) — set real and effective uid per POSIX rules
   - `setregid` (114) — set real and effective gid
   - `setresuid` (117) — set real, effective, saved uid (requires `CAP_SETUID` or matching existing IDs)
   - `setresgid` (119) — analogous for gid
   - `getresuid` (118) — copy `uid/euid/suid` to user pointers
   - `getresgid` (120) — copy `gid/egid/sgid` to user pointers
   - `setfsuid` (122) — set filesystem uid, return old fsuid
   - `setfsgid` (123) — set filesystem gid, return old fsgid

### C. VFS Special Bits (setuid / setgid / sticky)

8. In [src/kernel/fs/vfs.h](src/kernel/fs/vfs.h), add constants:
   - `VFS_S_ISUID  04000`
   - `VFS_S_ISGID  02000`
   - `VFS_S_ISVTX  01000`

9. Update `chmod` implementations to preserve all 12 bits (`mode & 07777`) instead of `mode & 0777`:
   - [src/kernel/fs/tmpfs/tmpfs.c](src/kernel/fs/tmpfs/tmpfs.c#L325) — `tmpfs_chmod`
   - [src/kernel/fs/ext2/ext2.c](src/kernel/fs/ext2/ext2.c#L591) — `ext2_chmod`
   - [src/kernel/syscall/file_syscalls.c](src/kernel/syscall/file_syscalls.c) — `sys_fchmodat` validation

10. In `sys_fchmodat` / `sys_fchmod`: if the caller is not the file owner and not `capable(CAP_FOWNER)`, strip `S_ISUID` and `S_ISGID` bits silently (Linux behavior). Also, on any successful `write()` to a setuid file, clear the setuid bit (add a `vnode_clear_suid_on_write()` helper called from `sys_write`/`sys_pwrite64`).

11. **Sticky bit enforcement**: In `sys_unlinkat` and `sys_rename` in [src/kernel/syscall/file_syscalls.c](src/kernel/syscall/file_syscalls.c), when the parent directory has `S_ISVTX` set, allow deletion only if `euid == file_owner || euid == dir_owner || capable(CAP_FOWNER)`.

### D. Setuid/Setgid-on-Exec

12. In `sys_execve` ([src/kernel/syscall/proc_syscalls.c](src/kernel/syscall/proc_syscalls.c)), **before** loading the ELF:
    - `vfs_stat()` the executable to get `mode`, `uid`, `gid`.
    - If `mode & S_ISUID`: set `new_euid = file_uid`, `new_suid = file_uid`.
    - If `mode & S_ISGID`: set `new_egid = file_gid`, `new_sgid = file_gid`.
    - After the ELF is loaded and the new address space is committed, apply the new credentials.
    - Set `AT_SECURE = 1` in the auxvec when setuid/setgid was applied (currently hardcoded to 0 at [line 711](src/kernel/syscall/proc_syscalls.c#L711)).
    - Push correct `AT_UID`, `AT_EUID`, `AT_GID`, `AT_EGID` from the (possibly changed) cred fields instead of the hardcoded 0 values at [lines 713-720](src/kernel/syscall/proc_syscalls.c#L713-L720).

13. **Capability transformation on exec** (Linux-compatible):
    - If the executable has `S_ISUID` and is owned by root, the new `cap_permitted` = old `cap_inheritable | cap_bounding` (simplified; full Linux rules involve file capabilities, but file-cap xattr support is out of scope — use this heuristic).
    - If `euid` transitions from 0 → non-0. clear `cap_effective` entirely.
    - If `euid` transitions from non-0 → 0 (shouldn't happen without setuid-root), grant full caps.
    - Respect `securebits` `SECBIT_NOROOT` if set (cap transitions suppressed for uid-0).

### E. Filesystem Permission Enforcement Refinements

14. In `check_vnode_access()` at [src/kernel/syscall/file_syscalls.c](src/kernel/syscall/file_syscalls.c#L85-L99):
    - Replace `cur->uid == 0` bypass with `capable(CAP_DAC_OVERRIDE)` for read/write and `capable(CAP_DAC_READ_SEARCH)` for read+search on directories.
    - Use `cur->cred.fsuid` / `cur->cred.fsgid` (not `euid`/`egid`) for filesystem owner/group comparisons.
    - For execute: root bypass should only apply if at least one execute bit is set on the file (Linux behavior).

15. In `sys_fchownat` / `sys_fchown` at [src/kernel/syscall/file_syscalls.c](src/kernel/syscall/file_syscalls.c):
    - Currently root-only. Change to: `capable(CAP_CHOWN)` for changing owner. Non-root users can change group only to a group they're a member of (`task_in_group()`), and only if they own the file.

16. **procfs ownership**: In [src/kernel/fs/procfs.c](src/kernel/fs/procfs.c), `procfs_stat()` should set `st->uid` / `st->gid` to the owning task's `euid`/`egid` for `/proc/<pid>/*` entries. Lookup the task by pid extracted from the path.

17. **devpts ownership**: In [src/kernel/drivers/devpts.c](src/kernel/drivers/devpts.c), when a PTY slave is opened via `/dev/ptmx`, set the slave's vnode `uid` to the opener's `fsuid` and `gid` to `tty` group (commonly gid 5 — add a `tty:x:5:` entry to [tools/rootfs/group](tools/rootfs/group)).

18. **devfs chmod/chown**: Add `devfs_chmod` and `devfs_chown` callbacks to [src/kernel/drivers/devfs.c](src/kernel/drivers/devfs.c) that update in-memory vnode fields (device nodes are virtual, so no persistence needed).

19. **FAT32 mount options**: In [src/kernel/fs/fat32/fat32.c](src/kernel/fs/fat32/fat32.c), add a `fat32_mount_opts` structure with `uid`, `gid`, `fmask`, `dmask` fields (defaulting to 0/0/022/022). Apply these when synthesizing vnodes at [line 390](src/kernel/fs/fat32/fat32.c#L390). This lets the mount command control apparent ownership.

### F. Signal & IPC Permission Checks

20. In `sc_kill()` at [src/kernel/syscall/syscall.c](src/kernel/syscall/syscall.c#L854-L880):
    - Before sending, check: `capable(CAP_KILL) || sender.euid == target.uid || sender.uid == target.uid || (sig == SIGCONT && sender.sid == target.sid)`.
    - Return `-EPERM` on failure.

21. In `signal_send()` at [src/kernel/ipc/signal.c](src/kernel/ipc/signal.c), add an optional `struct cred *sender_cred` parameter (NULL = kernel-sent, bypasses checks). Update all call sites.

22. In `sc_setpgid()` at [src/kernel/syscall/syscall.c](src/kernel/syscall/syscall.c):
    - Add permission check: a process can only set its own pgid or that of a child, and only within the same session. Return `-EPERM` otherwise (POSIX requirement).

### G. Network Privilege Checks

23. In `sys_bind()` handling in [src/kernel/syscall/net_syscalls.c](src/kernel/syscall/net_syscalls.c):
    - For `AF_INET`/`AF_INET6`: if port < 1024, require `capable(CAP_NET_BIND_SERVICE)`.

24. In `sys_socket()` for `SOCK_RAW`:
    - Require `capable(CAP_NET_RAW)`.

25. In Unix socket `connect()` at [src/kernel/net/unix.c](src/kernel/net/unix.c):
    - Check file permissions on the socket path vnode using `check_vnode_access()` with write permission.

### H. Capabilities Infrastructure

26. Define capability constants in [src/kernel/sched/cred.h](src/kernel/sched/cred.h):
    - `CAP_CHOWN` (0), `CAP_DAC_OVERRIDE` (1), `CAP_DAC_READ_SEARCH` (2), `CAP_FOWNER` (3), `CAP_FSETID` (4), `CAP_KILL` (5), `CAP_SETGID` (6), `CAP_SETUID` (7), `CAP_SETPCAP` (8), `CAP_NET_BIND_SERVICE` (10), `CAP_NET_RAW` (13), `CAP_SYS_CHROOT` (18), `CAP_SYS_ADMIN` (21), `CAP_SYS_RESOURCE` (24), `CAP_SYS_NICE` (23) — and any others needed. Use Linux numbering for ABI compat.
    - `capable(cred, cap)` macro: `((cred)->cap_effective & (1ULL << (cap)))`

27. Implement `capget` (125) and `capset` (126) syscalls:
    - `capget`: copies `cap_effective`, `cap_permitted`, `cap_inheritable` for the target pid.
    - `capset`: allows a process to **drop** capabilities from its own `cap_effective`/`cap_permitted`/`cap_inheritable` (never raise above `cap_permitted`). Changing another process's caps requires `CAP_SETPCAP`.

28. Update [src/kernel/fs/procfs.c](src/kernel/fs/procfs.c#L282) to report actual `cred.cap_effective`, `cap_permitted`, `cap_inheritable` instead of the faked values.

### I. `prctl()` Syscall (stub + key ops)

29. Add `sys_prctl` (157) to the syscall table. Implement at minimum:
    - `PR_SET_NO_NEW_PRIVS` / `PR_GET_NO_NEW_PRIVS` — store a `no_new_privs` flag in `task_t`. When set, `execve` must not honor setuid/setgid bits or transition capabilities upward.
    - `PR_SET_KEEPCAPS` / `PR_GET_KEEPCAPS` — controls whether caps are preserved across `setuid()` transitions.
    - `PR_SET_NAME` / `PR_GET_NAME` — set/get task `comm` name (already might exist; if not, add 16-byte `comm` field to `task_t`).
    - Return `-EINVAL` for unrecognized operations.

### J. `seccomp` Stub

30. Add a `seccomp_mode` field to `task_t` (or `struct cred`). Implement `sys_seccomp` (317) and `prctl(PR_SET_SECCOMP)`:
    - `SECCOMP_MODE_STRICT` — restrict to `read`, `write`, `exit`, `sigreturn` only.
    - `SECCOMP_MODE_FILTER` — accept but ignore BPF filter (stub: log warning and return 0). Full BPF filtering is out of scope.
    - Add a check at syscall entry in [src/kernel/syscall/syscall.c](src/kernel/syscall/syscall.c) dispatcher: if `seccomp_mode == STRICT` and syscall not in whitelist, deliver `SIGKILL`.

### K. Namespace Stubs

31. Add stub syscalls that return `-ENOSYS` or minimal behavior:
    - `unshare` (272) — return `-ENOSYS`
    - `setns` (308) — return `-ENOSYS`
    - `clone3` (435) with `CLONE_NEWUSER` etc. — return `-EINVAL` for namespace flags

    This ensures userspace detects "not supported" gracefully rather than crashing on unknown syscall numbers.

### L. Userspace Login Flow

32. Expand [tools/rootfs/passwd](tools/rootfs/passwd) with a regular user entry:
    ```
    root:x:0:0:root:/home/root:/bin/sh
    uday:x:1000:1000:Uday:/home/uday:/bin/sh
    nobody:x:65534:65534:nobody:/:/sbin/nologin
    ```

33. Expand [tools/rootfs/group](tools/rootfs/group):
    ```
    root:x:0:
    tty:x:5:
    users:x:100:uday
    uday:x:1000:
    nogroup:x:65534:
    ```

34. Modify [tools/init.c](tools/init.c) to implement a basic login flow:
    - Print a login prompt on the console.
    - Read a username.
    - Parse `/etc/passwd` (line-by-line, split on `:`) to find the matching entry.
    - (Optional: password check — skip for now or accept any password.)
    - Call `setgid(gid)`, `setgroups(...)` (parse `/etc/group` for supplementary groups), `setuid(uid)` to drop privileges.
    - `chdir(home_dir)`.
    - Set `HOME`, `USER`, `SHELL`, `LOGNAME` environment variables.
    - `exec(shell)` from the passwd entry.

35. Create a minimal `su` utility (new file `tools/su.c`):
    - If the caller is root (or has `CAP_SETUID`+`CAP_SETGID`), switch to the target user by calling `setresuid`/`setresgid`/`setgroups` and exec the target shell.
    - This serves as the primary way to test credential transitions.

### M. Makefile / Build Integration

36. Update the [makefile](makefile) to:
    - Compile the new `tools/su.c` and place the binary in the rootfs as `/bin/su` with mode `04755` (setuid root).
    - Ensure `/etc/passwd`, `/etc/group`, and `/etc/environment` are copied into the rootfs image.
    - Add the new source file [src/kernel/sched/cred.h](src/kernel/sched/cred.h) to the header dependency tracking.

---

**Verification**

1. **Boot test**: Kernel boots, init displays a login prompt, entering "root" drops to a root shell.
2. **Login as non-root**: Enter "uday" → shell runs with `uid=1000`, `gid=1000`. Verify with `id` command (if available) or read `/proc/self/status` for Uid/Gid lines.
3. **Permission denial**: As uid 1000, attempt to read `/dev/console` (mode 0600, owner root) → expect `-EACCES`.
4. **setuid-on-exec**: Place a setuid-root binary on the filesystem (`/bin/su`). Exec it as uid 1000 → verify `euid` becomes 0 inside the binary by reading `/proc/self/status`.
5. **Signal permission**: As uid 1000, attempt `kill(1, SIGTERM)` (init is uid 0) → expect `-EPERM`.
6. **Sticky bit**: Create `/tmp` with mode 1777. As user A, create a file. As user B, attempt to unlink it → expect `-EPERM`.
7. **Privileged port**: As uid 1000, attempt to bind TCP port 80 → expect `-EACCES`. As root → expect success.
8. **Capability drop**: As root, call `capset()` to drop `CAP_NET_BIND_SERVICE`, then try binding port 80 → expect `-EACCES`.
9. **seccomp strict**: Set `SECCOMP_MODE_STRICT`, call `open()` → process should be killed with `SIGKILL`.
10. **procfs accuracy**: `/proc/<pid>/status` should show distinct Real/Effective/Saved/FS UIDs after a `setresuid()` call.
11. **Regression**: All existing functionality (filesystem, networking, ELF loading, PTY, pipes) must still pass with the new credential model — root processes should behave identically since they start with full caps.

---

**Decisions**

- **Credential struct over bare fields**: Chose a `struct cred` embedded in `task_t` (not refcounted/shared like Linux). Linux uses `struct cred __rcu *cred` with RCU — that's an optimization for concurrent reads; EXO_OS's simpler model is fine at this scale.
- **Capabilities via bitmask, no file capabilities**: Linux stores per-file capabilities in xattrs. We skip that and only do setuid-root → full-caps heuristic + in-process `capset()`. This covers 95% of real usage.
- **seccomp filter = stub**: Full BPF filtering requires a BPF VM. We implement `SECCOMP_MODE_STRICT` properly and accept-but-ignore filter mode. Enough for `prctl(PR_SET_NO_NEW_PRIVS)` + detection.
- **No password hashing**: The login flow accepts any password (or no password). Real auth with `/etc/shadow` and crypt() can be added later.
- **Linux syscall numbers**: All new syscalls use the exact x86_64 Linux syscall numbers for ABI compatibility with musl/glibc.