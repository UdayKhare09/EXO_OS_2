# EXO_OS

EXO_OS is a 64-bit hobby operating system for x86_64 with a Linux-like syscall ABI, UEFI boot via Limine, a custom kernel, and a generated GPT disk image containing EFI + ext2 root + ext2 data partitions.

The project currently includes:

- A freestanding kernel (processes, scheduler, VFS, memory manager, syscalls)
- Filesystems: ext2, FAT32, tmpfs, procfs, sysfs, devfs, devpts
- Device and platform support: AHCI, virtio-blk, virtio-net, e1000, xHCI USB, framebuffer console
- Networking stack: Ethernet, ARP, IPv4, ICMP, UDP, TCP, AF_UNIX sockets
- Linux-style credentials/capabilities and permission model (in progress toward broader parity)
- A minimal ext2 rootfs with staged host userspace tools, glibc runtime support, login init, and setuid `su`

---

## Repository Layout

- `src/kernel/` — kernel code
  - `arch/x86_64/` — CPU/platform bring-up, interrupt/APIC/SMP/ACPI
  - `mm/` — PMM/VMM/kmalloc/page cache
  - `sched/` — tasking/scheduler/wait queues/credentials
  - `syscall/` — syscall dispatcher + syscall implementations
  - `fs/` — VFS + filesystem drivers
  - `drivers/` — storage/net/usb/input/devfs/devpts/pty
  - `net/` — protocol stack + sockets
  - `ipc/` — signals, pipes, IPC primitives
- `src/boot/limine.conf` — boot menu and kernel entry
- `tools/` — userspace programs and utilities (`init`, `su`, smoke tests, font generator)
- `tools/rootfs/` — rootfs config files (`passwd`, `group`, `environment`, `profile`)
- `makefile` — full build, image assembly, and QEMU run targets
- `build/` — generated artifacts (kernel ELF, partitions, final disk image)

---

## Architecture Snapshot

Boot flow (high level):

1. UEFI firmware loads Limine
2. Limine loads `build/exo.elf`
3. Kernel initializes memory, interrupts, APIC/SMP, VFS/drivers/network
4. Storage init mounts ext2 root (`/`) and FAT32 EFI (`/boot`) when available
5. Kernel starts userspace `init` (`/sbin/init`)
6. `init` prompts login and launches the shell

Kernel entry is in `src/kernel/main.c`.

---

## Current Security/Credentials Model

EXO_OS now uses a Linux-like credential structure per task (`src/kernel/sched/cred.h`) including:

- Real/effective/saved/fs UIDs and GIDs
- Supplementary groups
- Capability bitmasks (`effective/permitted/inheritable/bounding`)
- `securebits` support (including `SECBIT_NOROOT` handling paths)

Implemented behavior includes:

- `set*uid`/`set*gid` families (`setre*`, `setres*`, `setfs*`)
- `capget`/`capset`
- setuid/setgid exec transitions with auxv identity updates
- sticky bit enforcement on unlink/rename
- privileged net checks (`CAP_NET_RAW`, `CAP_NET_BIND_SERVICE`)
- `prctl` subset (`NO_NEW_PRIVS`, `KEEPCAPS`, `SET/GET_NAME`, seccomp mode ops)
- `seccomp` strict mode syscall gate + filter-mode stub
- namespace stubs (`unshare`, `setns`) and `clone3` namespace-flag rejection

---

## Build Requirements (Host)

You need a Linux host with at least:

- `clang`, `ld.lld`, `llvm-objcopy`, `nasm`, `cc`
- `qemu-system-x86_64`
- `mtools` (`mmd`, `mcopy`), `mkfs.fat`
- `e2fsprogs` (`mkfs.ext2`, `debugfs`)
- `sgdisk` (from `gdisk`)
- `git`, `curl`
- OVMF files:
  - `/usr/share/edk2/x64/OVMF_CODE.4m.fd`
  - `/usr/share/edk2/x64/OVMF_VARS.4m.fd`
- `gcc`/glibc runtime pieces available on the host (`libc.so.6`, `ld-linux-x86-64.so.2`, headers, binutils)

---

## Build

From repository root:

```bash
make all -j4
```

Main outputs:

- `build/exo.elf` — kernel ELF
- `build/efi.img` — EFI FAT partition
- `build/root.img` — ext2 root partition
- `build/data.img` — ext2 data partition
- `build/exo.img` — final GPT disk image

---

## Run in QEMU

```bash
make run
```

This uses OVMF + q35 + KVM and opens VGA framebuffer output with serial also on stdio.

Debug stop-at-entry mode:

```bash
make run-debug
```

(`-s -S` for GDB attach)

---

## Login and Userspace Notes

Rootfs users are configured in:

- `tools/rootfs/passwd`
- `tools/rootfs/group`

Current default accounts include `root`, `uday`, and `nobody`.

`/sbin/init` (`tools/init.c`) performs a simple login flow:

- prompt for username
- parse `/etc/passwd`
- set supplementary groups + gid/uid
- set `HOME`, `USER`, `LOGNAME`, `SHELL`
- exec user shell

Setuid `su` is installed as `/bin/su` (mode `04755`) and built from `tools/su.c`.

---

## Quick Validation Checklist

After boot:

1. Login as `uday`
2. Check identity/caps:
   ```sh
   cat /proc/self/status | grep -E 'Uid|Gid|Groups|Cap(Inh|Prm|Eff|Bnd)|NoNewPrivs|Seccomp'
   ```
3. Test setuid transition:
   ```sh
   /bin/su root
   cat /proc/self/status | grep -E 'Uid|CapEff'
   ```
4. Test privileged bind behavior (if `nc` applet/binary present):
   - as non-root: bind port 80 should fail
   - as root: bind port 80 should pass

---

## Networking Helpers

- Kernel includes in-tree TCP/UDP/IP stack and UNIX domain sockets.
- A host-side helper script exists at `tools/exo_nettest.py`.

---

## Cleaning

```bash
make clean
make distclean
```

---

## Project Status

This is an active experimental OS codebase. Interfaces and behavior are evolving quickly, especially around Linux compatibility and security semantics.
