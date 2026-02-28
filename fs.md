Plan: Full Linux-like Filesystem Hierarchy (FHS) Implementation
TL;DR: EXO_OS already has a solid VFS core, tmpfs, ext2, devfs, procfs, sysfs, and a rich syscall layer. The goal is to fill every gap to produce a Linux-compatible dev, proc, sys, run, pts hierarchy — formatted byte-for-byte like Linux so that standard tools (procps, iproute2, fish, glibc mlibc, ps, top, ip, etc.) work without modification.

The work is organized into 6 phases executed sequentially, each buildable/testable independently.

Phase 1 — Foundation Fixes (prerequisite for everything)
These are cheap fixes that unblock all later phases.

Increase VFS_MAX_MOUNTS in vfs.h from 16 → 32 — /, dev, pts, shm, tmp, run, proc, sys, boot = 9 mounts minimum.
Grow PROCFS_BUF_SIZE in procfs.c from 4 096 → 65 536 bytes — process VMAs can easily overflow 4 KB.
Add exe_path[512] to task_t in task.h and populate it in sys_execve in proc_syscalls.c — fixes /proc/<pid>/exe readlink (currently hardcoded to sh).
Mount run as tmpfs at boot in main.c: vfs_mkdir("/run", 0755) + tmpfs mount — Linux daemons (udev, dbus, NetworkManager) write PID files here.
Create etc on tmpfs before ext2 mount in main.c so early-boot lookup of environment never hits ENOENT.
vfs_open() helper: add a path-based vfs_open(path, flags, mode) → file_t* wrapper in vfs.c / vfs.h so callers stop doing vfs_lookup + file_alloc manually.
Phase 2 — TTY / Terminal I/O (ioctl POSIX compliance)
Without this, interactive shells (fish, bash) crash immediately on tcgetattr() / tcsetattr().

Define a tty_state_t struct in a new src/kernel/drivers/tty.h: holds struct termios (Linux ABI-compatible: c_iflag, c_oflag, c_cflag, c_lflag, c_cc[NCCS]) and struct winsize (ws_row, ws_col, ws_xpixel, ws_ypixel). Seed ws_col/ws_row from the fbcon framebuffer dimensions at boot.
Wire f_ops->ioctl on tty in devfs.c to handle:
TCGETS (0x5401) — copy tty_state_t.termios to userspace
TCSETS/TCSETSW/TCSETSF (0x5402–0x5404) — copy from userspace into tty_state_t
TIOCGWINSZ (0x5413) — copy winsize to user
TIOCSWINSZ (0x5414) — update winsize, optionally deliver SIGWINCH
TIOCSCTTY (0x540E) — set controlling terminal for current session
TIOCSPGRP (0x5410) / TIOCGPGRP (0x540F) — set/get foreground process group
TIOCNOTTY (0x5422) — detach from controlling terminal
Route sys_ioctl in file_syscalls.c through f->f_ops->ioctl for all fds (currently only socket fds reach ioctl). Non-socket non-TTY fds should return ENOTTY.
Add console in devfs.c as a static entry pointing to the same TTY read/write ops — init systems open console to set up stdio before spawning getty.
Phase 3 — Pseudoterminals (devpts + ptmx)
Required for SSH, terminal multiplexers (tmux), and sub-shell spawning in terminal emulators.

Create src/kernel/drivers/pty.h / pty.c implementing:
A pty_pair_t struct: master ringbuf + slave ringbuf (each 4 096 bytes), struct termios, struct winsize, slave index N, a waitq for each side.
pty_open_master() → returns a file_t* for the master side (PTM).
pty_open_slave(N) → returns a file_t* for the slave side (PTS N).
Master read drains slave→master buffer; master write applies line discipline and puts bytes in master→slave buffer.
Line discipline (canonical mode): process c_cc characters (VINTR → SIGINT, VEOF → EOF, VERASE/backspace, VKILL, VSWTC), respect ICANON/ECHO/ISIG/OPOST/ONLCR/ICRNL flags.
ptmx in devfs.c: opening it calls pty_open_master(), returns the master fd. ioctl(TIOCGPTN) on the master returns slave index N; ioctl(TIOCSPTLCK, 0) unlocks it (glibc calls these via posix_openpt + grantpt + unlockpt).
New devpts filesystem — src/kernel/drivers/devpts.c: a minimal synthetic fs mounted at pts. Its readdir enumerates allocated PTY pairs; lookup("N") calls pty_open_slave(N). Register as fs type "devpts" and mount at boot: mount("devpts", "/dev/pts", "devpts", ...).
Create pts directory in main.c and mount devpts there at boot.
Phase 4 — procfs Completion (Linux-exact format)
All output formats should match Linux kernel 5.x output that procps / ps / top expect.

/proc/<pid>/fd/ directory: in procfs.c, add a special PROC_NODE_FD_DIR type. readdir iterates task->fd_table[0..255] emitting entries "0", "1", … for each non-NULL slot. lookup("N") returns a symlink vnode whose readlink returns file->path (for regular files) or "socket:[ino]" / "pipe:[ino]" / "anon_inode:[eventfd]" for other types — matching Linux's exact notation.
/proc/<pid>/environ: in procfs.c, expose NUL-separated environment. Requires storing char **envp (or the raw env block pointer + length) in task_t at exec time in proc_syscalls.c.
/proc/<pid>/io: bytes_read, bytes_written, syscr, syscw — update counters in task_t at every sys_read/sys_write call. Format matches Linux exactly.
/proc/<pid>/cgroup: emit "0::/\n" (cgroups v2 stub) — many tools check this exists.
/proc/<pid>/oom_score / /proc/<pid>/oom_score_adj: emit "0\n" / "0\n".
sys subtree: implement as a sub-directory tree inside procfs. Key nodes (all writable where Linux allows writes):
hostname — r/w, backed by a global char g_hostname[64]; sys_uname should read this too.
ostype, osrelease, version, pid_max, threads-max, overflowuid, overflowgid
ngroups_max, printk, dmesg_restrict
file-max, file-nr (3 tab-separated values), nr_open, pipe-max-size, overflowuid
overcommit_memory, overcommit_ratio, swappiness — stubs returning Linux defaults (0, 50, 60)
somaxconn, rmem_max, wmem_max, rmem_default, wmem_default
ip_forward, ip_local_port_range, tcp_rmem, tcp_wmem, tcp_fin_timeout
disable_ipv6 → emit "1\n"
net subtree (matching Linux net):
tcp — one row per open TCP socket, 17-column Linux format (hex local/remote addr:port, state, tx/rx queue, etc.)
udp — same format for UDP sockets
arp — IP address, HW type, flags, HW address, mask, device — matches arp -n parsing
route — Iface, Destination, Gateway, Flags, RefCnt, Use, Metric, Mask, MTU, Window, IRTT (hex, like Linux)
if_inet6 — per-interface IPv6 stub: omit or emit nothing
dev (already exists) — verify it matches Linux's 17-field format exactly
fib_trie, fib_triestat — stubs returning minimal correct output
interrupts: emit header line + one row per IRQ vector with counts per CPU + chip name + action name (matches cat /proc/interrupts).
iomem: emit MMIO regions from the limine memory map.
ioports: emit x86 I/O port ranges (keyboard 0060-006f, etc.) — static list.
buddyinfo: emit a single zone line (Node 0, zone Normal) with 11 tab-separated bucket counts (stubs of zero).
slabinfo: emit the 2-line header + one row for each kmalloc slab. Matches slabinfo v2.1 format.
zoneinfo: short stub with one zone per NUMA node.
crypto: emit a few dummy algorithm entries for tools that enumerate supported ciphers.
diskstats: per-block-device I/O statistics, 14-field Linux format — hook counters into the blkdev_t read/write paths.
Phase 5 — sysfs Completion
/sys/class/net/<ifname>/ — per netdev_t in sysfs.c:
address → MAC as xx:xx:xx:xx:xx:xx\n
mtu → "1500\n"
carrier → "1\n" if link up
operstate → "up\n" / "down\n"
type → "1\n" (ARPHRD_ETHER)
flags → hex IFF_UP | IFF_BROADCAST | IFF_RUNNING etc.
tx_queue_len → "1000\n"
speed → "1000\n", duplex → "full\n"
ifindex → integer string
uevent → INTERFACE=<name>\nIFINDEX=N\n
statistics/ subdirectory: rx_bytes, tx_bytes, rx_packets, tx_packets, rx_errors, tx_errors, rx_dropped, tx_dropped
/sys/class/block/<dev>/ per blkdev_t:
size (in 512-byte sectors), dev (major:minor as "254:N\n"), removable, ro, uevent
queue/ subdir: logical_block_size ("512\n"), physical_block_size, scheduler ("none\n"), rotational ("0\n")
tty0: active → "tty0\n", uevent.
event0: name, uevent — maps to the keyboard input device.
/sys/bus/pci/devices/<dev>/ — add missing files: vendor (4-digit hex), device, class, irq, resource, config (256-byte binary PCI config space) to the existing PCI entries in sysfs.c.
mm: transparent_hugepage/enabled → emit "always [madvise] never\n".
acpi: stub directory; many programs just stat it.
state writable node: echo mem > /sys/power/state → invoke a kernel power_suspend() stub.
Phase 6 — Missing Syscalls + AF_UNIX + epoll
truncate() (SYS 76) in file_syscalls.c: path → vfs_lookup → ops->truncate.
link() (SYS 86): add link op to fs_ops_t in vfs.h; implement in tmpfs and ext2.
mknod() (SYS 133): add to file_syscalls; devfs must handle create() to register a new static device node by major/minor.
statfs() / fstatfs() (SYS 137/138): add statfs op to fs_ops_t; each filesystem returns struct statfs with f_type (magic number), f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_fsid, f_namelen, f_frsize. Linux magic numbers: tmpfs=0x01021994, ext2=0xEF53, proc=0x9FA0, sysfs=0x62656572, devpts=0x1CD1, devtmpfs=0x1373.
flock() (SYS 73): advisory lock table in VFS layer (per-vnode linked list of flock_entry_t), blocking via waitq.
fallocate() (SYS 285): FALLOC_FL_KEEP_SIZE + default mode — implement for ext2 and tmpfs.
epoll_create1() (SYS 291) / epoll_ctl() (SYS 233) / epoll_wait() (SYS 232): new src/kernel/fs/epoll.c. An epoll fd is a generic file_t with f_ops. Internally an epoll_t holds a list of epoll_entry_t (target fd, events, data). epoll_wait iterates ready entries or sleeps on a waitq; f_ops->poll on each monitored fd checks readiness. This enables async I/O for nginx, node.js, etc.
AF_UNIX domain sockets in a new src/kernel/net/unix.c:
socket(AF_UNIX, SOCK_STREAM, 0) / SOCK_DGRAM
bind(): creates a vnode of type VSOCK in the VFS at the bind path (on tmpfs/ext2)
connect(): by path, wakes the listening socket
accept(): creates a connected pair (two cross-linked unidirectional ring bufs)
sendmsg/recvmsg with SCM_RIGHTS (fd passing) and SCM_CREDENTIALS (struct ucred)
All backed by the existing file_ops_t mechanism — sock->f_ops delegate to unix ops
socketpair(AF_UNIX, ...): creates a connected pair without bind (used heavily by D-Bus and glibc)
sendmsg / recvmsg (SYS 46/47): already registered as stubs; implement for both inet and unix sockets.
Verification
Execute each phase with a QEMU boot (make qemu or equivalent from the makefile):

Phase 1: ls /dev /proc /sys /run /etc should all succeed; /proc/<pid>/exe should show the real binary path.
Phase 2: stty -a inside the OS should print full termios settings; fish/bash interactive mode should not crash.
Phase 3: python3 -c "import pty; pty.spawn('/bin/sh')" or expect command should work; 0 should appear after opening ptmx.
Phase 4: cat /proc/self/fd/0 → symlink to tty; cat /proc/net/tcp should print socket table; sysctl -a should enumerate sys without errors.
Phase 5: ip link show / ip addr (using net); ls /sys/class/block/; lspci reading devices.
Phase 6: df -h (statfs); touch a; ln a b (hard link); strace using epoll; D-Bus startup (AF_UNIX + epoll); flock shell command.
Decisions

All procfs/sysfs content formatted to match Linux kernel 5.15 LTS output (procps, iproute2, mlibc field-parsers work without patching).
devpts is a first-class pseudo-filesystem (its own fs_type_t registration) rather than a devfs sub-directory — matches Linux architecture.
epoll uses existing f_ops->poll hook as the readiness check primitive — no separate event-poll abstraction needed.
AF_UNIX backed by tmpfs vnodes for binding (no separate sockfs) — simpler and sufficient for SOCK_STREAM/DGRAM.
VFS_MAX_MOUNTS bumped to 32 in Phase 1 to accommodate all new mounts upfront.