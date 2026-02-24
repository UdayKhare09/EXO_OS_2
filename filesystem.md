# Plan: Comprehensive Filesystem Stack (VFS + GPT + FAT32 + ext2 + tmpfs)

**Summary:** The kernel has no storage drivers, no block device layer, no VFS, and no filesystem code at all. We'll build the full stack from scratch in a Linux-like layered architecture: block device abstraction → storage drivers (VirtIO-blk + AHCI, both supported natively by QEMU for passthrough) → GPT parser → buffer cache → VFS core → FAT32 r/w + ext2 r/w + tmpfs → POSIX fd table (per-task) → syscall ABI (Linux-compatible INT 0x80 / vector 0x80). All sources drop into `src/kernel/` and are auto-discovered by the Makefile's `find`.

---

## Phase 1 — Block Device Abstraction (`src/kernel/drivers/storage/blkdev.h` + `blkdev.c`)

1. Define `blkdev_t` with ops vtable: `read_blocks(dev, lba, count, buf)`, `write_blocks`, `get_block_count`, `get_block_size`, `flush`; plus `char name[32]`, `uint64_t dev_id`, `void *priv`
2. Global registry `g_blkdevs[16]` with `blkdev_register(blkdev_t*)`, `blkdev_get(id)`, `blkdev_find_by_name(name)` — keeps device numbering like `/dev/sda`, `/dev/vda`
3. `blkdev_read()` / `blkdev_write()` wrappers that go through the ops and the buffer cache

## Phase 2 — Buffer Cache (`src/kernel/fs/bcache.h` + `bcache.c`)

4. `bcache_buf_t` with `(dev_id, block_no)` key, `uint8_t *data`, `dirty` flag, `refcount`, LRU list links; backed by `kmalloc`
5. Hash table (256 buckets, chained) for O(1) lookup; `bcache_get(dev, block_no)`, `bcache_release(buf)`, `bcache_flush_dev(dev)`, `bcache_sync_all()`; LRU eviction when pool is full
6. All storage drivers and FS drivers go through bcache — they never call `blkdev_read` directly

## Phase 3 — VirtIO-BLK Driver (`src/kernel/drivers/storage/virtio_blk.h` + `virtio_blk.c`)

7. PCI scan for vendor `0x1AF4` + device `0x1001` (legacy) or `0x1042` (modern) using existing `pci_find()`; enable BusMaster via `pci_enable_device()`
8. Map BAR0 (I/O or MMIO) via `vmm_mmio_map`; negotiate VirtIO feature bits (capacity, block size, flush), negotiate `VIRTIO_F_VERSION_1`
9. Set up the single requestq (virtqueue 0): allocate descriptor table + available ring + used ring via `pmm_alloc_pages`; write queue PFN/addresses into device config
10. `virtio_blk_rw(dev, lba, count, buf, write)` — enqueue 3-descriptor chain (header → data → status), kick queue, spin on used ring (or block task via `sched_block`/`sched_unblock` with ISR wakeup)
11. Register as `blkdev_t` named `"vda"` (VirtIO Disk A pattern)

## Phase 4 — AHCI Driver (`src/kernel/drivers/storage/ahci.h` + `ahci.c`)

12. PCI scan: class `0x01`, subclass `0x06`; map ABAR (BAR5) MMIO via `vmm_mmio_map`; enable AHCI global mode in GHC, set AE bit
13. Per-port: check `PxSSTS` for device presence; stop port DMA (PxCMD clear ST+FRE); allocate command list (32 slots × 32 bytes), FIS receive buffer (256 bytes) via `pmm_alloc_pages`; set PxCLB + PxFB; restart DMA
14. Implement `ahci_rw(port, lba, count, buf, write)` using a command table + PRDT (one PRD per 4 KB chunk); set DMA bit + NCQ if available; wait for completion via PxIS polling (or ISR via `idt_register_handler`)
15. Identify drives with ATA IDENTIFY; register each present port as `blkdev_t` named `"sda"`, `"sdb"`, etc.

## Phase 5 — GPT Partition Parser (`src/kernel/fs/gpt.h` + `gpt.c`)

16. `gpt_header_t` packed struct (LBA 1, 92-byte primary header); validate signature `"EFI PART"`, revision, CRC32 (implement a 256-entry CRC32 table in `gpt.c`)
17. `gpt_entry_t` packed struct (128 bytes each); scan up to 128 entries from the partition array LBA
18. `gpt_scan(blkdev_t *dev, gpt_partition_t *out, int max)` → populates array of `{ blkdev_t *dev; uint64_t lba_start; uint64_t lba_end; guid_t type_guid; uint16_t label[36]; }` with known GUIDs: Linux data (`0FC63DAF…`), EFI System (`C12A7328…`), Microsoft Basic Data (`EBD0A0A2…`)
19. `part_blkdev_create(parent, gpt_partition_t*)` — wraps parent `blkdev_t`, offsets all LBA reads/writes by `lba_start`; registers as `"sda1"`, `"vda1"`, etc.

## Phase 6 — VFS Core (`src/kernel/fs/vfs.h` + `vfs.c` + `path.c`)

20. Define `vnode_t` (ino, size, mode, uid/gid, timestamps, `fs_ops_t*`, `void *fs_data`, `mount_t*`, refcount), `dirent_t` (ino + name[256])
21. Define `fs_ops_t` vtable: `lookup`, `open`, `close`, `read`, `write`, `readdir`, `mkdir`, `create`, `unlink`, `rename`, `stat`, `symlink`, `readlink`, `truncate`, `sync`, `destroy`
22. `mount_t` with `path[256]`, root `vnode_t*`, `fs_instance*`; global `g_mounts[16]`; `vfs_mount(path, blkdev_t*, fstype)`, `vfs_unmount(path)`, `vfs_get_root()`
23. `vfs_lookup(path)` — walk path components: split on `/`, resolve each segment via `fs_ops.lookup`, handle `..` (go up to parent), handle mount-point crossing, handle symlinks (up to 8 levels via recursion guard)
24. `path_normalize(in, out)` — remove `//`, `.` and `..` without syscall; `path_join(base, rel, out)` for relative paths; `path_dirname` / `path_basename`

## Phase 7 — File Descriptor Table (`src/kernel/fs/fd.h` + `fd.c`)

25. `file_t` struct: `vnode_t*`, `uint64_t offset`, `int flags` (`O_RDONLY=0`, `O_WRONLY=1`, `O_RDWR=2`, `O_APPEND`, `O_NONBLOCK`, `O_CREAT`, `O_TRUNC`, `O_DIRECTORY`), `uint32_t refcount`
26. Extend `task_t` in `src/kernel/sched/task.h`: add `file_t *fd_table[256]` and `char cwd[256]`; update `task_create` to allocate and zero the fd table; update `task_destroy` to close all open fds
27. `fd_alloc(task, file)` → returns lowest free fd ≥ 0; `fd_get(task, fd)` → validates and returns `file_t*`; `fd_close(task, fd)` → decrements refcount, calls `vnode->ops->close` if 0; `fd_dup(task, old_fd)` → inc refcount + alloc new slot

## Phase 8 — FAT32 Driver (`src/kernel/fs/fat32/fat32.h` + `fat32.c`)

28. Parse BPB (BIOS Parameter Block) from sector 0: sectors per cluster, reserved sectors, FAT count, root cluster, FAT32 signature (`0x28/0x29`); compute FAT LBA, data LBA, root dir start cluster
29. FAT chain traversal: `fat32_next_cluster(fs, cluster)` reads FAT sector via bcache; cluster-to-LBA conversion `(cluster - 2) * spc + data_lba`
30. Directory: iterate through 32-byte entries; handle LFN (attribute `0x0F`): collect LFN entries in reverse, assemble UTF-16 → ASCII/UTF-8 name; skip deleted (`0xE5`) and end-of-dir (`0x00`)
31. `fat32_read(vnode, buf, len, off)` — compute starting cluster from offset, follow chain, read full/partial clusters via bcache
32. `fat32_write(vnode, buf, len, off)` — allocate new clusters from FAT bitmap when needed, write data clusters, update FAT chain, update directory entry size + mtime
33. `fat32_mkdir`, `fat32_create`, `fat32_unlink`, `fat32_rename` — allocate cluster for new dir (`.` and `..` entries), write directory entries, update parent
34. Register as `fs_type_t FAT32`; `fat32_mount(blkdev_t*)` returns a `vnode_t*` root; auto-detect by checking BPB signature

## Phase 9 — ext2 Driver (`src/kernel/fs/ext2/ext2.h` + `ext2.c`)

35. Parse superblock (block 1, `0xEF53` magic); extract `s_blocks_per_group`, `s_inodes_per_group`, `s_log_block_size` (block size = 1024 << log)
36. Block Group Descriptor Table: compute BGD LBA; `ext2_get_inode(fs, ino)` → find BGD for group `(ino-1) / inodes_per_group`, read inode table block, offset to inode
37. Block mapping: direct (0–11), single indirect (12), double indirect (13), triple indirect (14); `ext2_bmap(inode, logical_block)` returns physical block number
38. Directory: `ext2_dirent_t` with inode + rec_len + name_len + file_type + name (variable); linear scan of dir blocks
39. `ext2_read` / `ext2_write` / `ext2_create` / `ext2_mkdir` / `ext2_unlink`; block allocation from block bitmap (read bitmap block, find clear bit, set it, write back via bcache)
40. Register as `fs_type_t EXT2`; detect by superblock magic; mount → return root inode (ino=2)

## Phase 10 — tmpfs (`src/kernel/fs/tmpfs/tmpfs.h` + `tmpfs.c`)

41. `tmpfs_inode_t` with `vnode_t` embedded, `uint8_t *data; size_t capacity; size_t size` for regular files; `tmpfs_dirent_t` linked list for dirs
42. All ops in pure `kmalloc`/`kfree` — no block device; `tmpfs_read` copies from `data` buffer; `tmpfs_write` grows buffer with `kmalloc`/`memcpy` as needed
43. Mount at `/` initially (root tmpfs) and `/tmp`; `/dev`, `/proc` stubs as empty tmpfs dirs

## Phase 11 — Syscall ABI (`src/kernel/syscall/syscall.h` + `syscall.c` + `file_syscalls.c`)

44. Register IDT vector `0x80` via `idt_register_handler` with `isr_handler_t syscall_dispatch`; read syscall number from `regs->rax`; args from `rdi, rsi, rdx, r10, r8, r9` (Linux x86-64 ABI); return value in `rax`
45. Syscall table (function pointer array, Linux ABI numbers): `0=read`, `1=write`, `2=open`, `3=close`, `4=stat`, `5=fstat`, `6=lstat`, `8=lseek`, `9=mmap`, `21=access`, `22=pipe`, `32=dup`, `33=dup2`, `59=execve`, `60=exit`, `79=getcwd`, `80=chdir`, `83=mkdir`, `84=rmdir`, `85=creat`, `86=link`, `87=unlink`, `88=symlink`, `89=readlink`, `90=chmod`, `133=mknod`, `157=prctl`, `217=getdents64`
46. Implement each syscall calling down into VFS: `sys_open` → `vfs_lookup` + `fd_alloc`; `sys_read` → `fd_get` + `vfs_read`; `sys_write` → `fd_get` + `vfs_write`; `sys_close` → `fd_close`; `sys_stat` → `vfs_lookup` + `fs_ops.stat`; `sys_getdents64` → `fs_ops.readdir` in loop; `sys_mkdir`, `sys_unlink`, `sys_rename`, `sys_getcwd`, `sys_chdir`

## Phase 12 — Integration + Makefile disk image (`makefile` + `src/kernel/main.c`)

47. Add `fs_init()` in `main.c` after `kmalloc_init()`: `bcache_init()` → `tmpfs_init()` → `vfs_init()` → mount tmpfs at `/`, create `/dev /tmp /proc /mnt` dirs
48. Spawn `"storage-init"` task (after PCI enumerate, before USB): scans VirtIO-blk + AHCI, registers `blkdev_t`s, scans GPT, auto-mounts FAT32 partitions at `/boot/efi` and ext2 at `/` (or `/mnt/disk`)
49. Add `disk.img` creation target in `makefile`: `dd` → `sgdisk` (GPT with EFI + Linux partitions) → `mkfs.fat` → `mkfs.ext2`; update QEMU `-drive` flags to add `file=build/disk.img,if=virtio,format=raw`; optionally add AHCI: `-device ahci,id=ahci0 -device ide-hd,drive=d1,bus=ahci0.0`

---

## New File Tree

```
src/kernel/
├── drivers/storage/
│   ├── blkdev.{h,c}
│   ├── virtio_blk.{h,c}
│   └── ahci.{h,c}
├── fs/
│   ├── vfs.{h,c}
│   ├── fd.{h,c}
│   ├── path.c
│   ├── gpt.{h,c}
│   ├── bcache.{h,c}
│   ├── fat32/fat32.{h,c}
│   ├── ext2/ext2.{h,c}
│   └── tmpfs/tmpfs.{h,c}
└── syscall/
    ├── syscall.{h,c}
    └── file_syscalls.c
```

~20 new source files, ~5 500–6 000 lines total. No Makefile edits needed for source files.

---

## Decisions

- **VirtIO-blk (primary) + AHCI (secondary):** VirtIO-blk is QEMU's fastest path; AHCI covers `qemu -device ahci` SATA passthrough and real hardware
- **Buffer cache between block layer and filesystems:** avoids redundant sector reads; critical for ext2 block group lookups
- **Linux ABI syscall numbers:** makes it possible to later run musl-libc ELF binaries compiled for Linux without patching syscall tables
- **tmpfs as initial `/`:** kernel can run without any disk present; storage mounts overlay later
- **LFN support in FAT32:** required for any real-world FAT32 usage (all modern FAT32 disks use LFN)

---

## Verification

- Build: `make all` — zero new errors/warnings expected (all headers `#pragma once`, all enums typed)
- QEMU boot with disk image: `make run` — serial log should show bcache init, virtio-blk detected (`"vda: 204800 blocks @ 512B"`), GPT scan (`"vda1: EFI System, vda2: Linux ext2"`), mounts succeed
- Shell command smoke tests via existing `shell.c`: add `ls`, `cat`, `mkdir`, `stat` shell commands that call through VFS
- Stress: write a test task that creates 1000 files in tmpfs, reads them back, then does the same on FAT32 and ext2 partitions; verify no kmalloc leaks via `pmm_print_stats`
