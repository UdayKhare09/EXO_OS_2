#pragma once
/* fs/ext2/ext2.h — ext2 filesystem driver public interface */
#include "fs/vfs.h"

extern fs_ops_t g_ext2_ops;
void ext2_register(void);
