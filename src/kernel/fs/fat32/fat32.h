/* fs/fat32/fat32.h — FAT32 filesystem driver */
#pragma once
#include "fs/vfs.h"

/* Register the FAT32 filesystem type with the VFS. Call once at init. */
void fat32_register(void);

/* FAT32 fs_ops vtable (exported for vfs_register_fs). */
extern fs_ops_t g_fat32_ops;

/* Configure default FAT32 mount options for the next FAT32 mount call.
 * opts supports comma-separated key=value pairs: uid,gid,fmask,dmask. */
void fat32_set_mount_opts(const char *opts);
