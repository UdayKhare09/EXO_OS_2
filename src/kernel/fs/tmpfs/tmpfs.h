#pragma once
/* fs/tmpfs/tmpfs.h — in-memory temporary filesystem public interface */
#include "fs/vfs.h"

extern fs_ops_t g_tmpfs_ops;
void tmpfs_register(void);
