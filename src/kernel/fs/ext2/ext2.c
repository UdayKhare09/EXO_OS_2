/* fs/ext2/ext2.c — ext2 read-write filesystem driver
 *
 * Supports: superblock + BGD parsing, inode read, direct + single/double/
 *           triple indirect block mapping, linear directory iteration,
 *           file read/write, create, mkdir, unlink, rename.
 *
 * Block I/O is done through the buffer cache (bcache) at 512-byte granularity.
 * Each ext2 block is assembled from one or more sectors when needed.
 */
#include "ext2.h"
#include "fs/vfs.h"
#include "fs/bcache.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

#include <stdint.h>
#include <stdbool.h>

/* ── Constants ───────────────────────────────────────────────────────────── */
#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2
#define EXT2_SECTOR_SIZE    512

/* Inode type bits (i_mode) */
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFLNK  0xA000
#define EXT2_IFMT     0xF000

/* Directory entry file_type */
#define EXT2_DT_UNKNOWN 0
#define EXT2_DT_REG     1
#define EXT2_DT_DIR     2
#define EXT2_DT_LNK     7

/* ── On-disk superblock (at byte offset 1024) ─────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;    /* 0 for 4K blocks, 1 for 1K blocks    */
    uint32_t s_log_block_size;      /* block_size = 1024 << s_log_block_size */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;               /* 0xEF53 */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* Extended superblock (rev >= 1) */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t _padding;
    /* ... more fields follow but we don't need them */
} ext2_superblock_t;

/* ── Block Group Descriptor (32 bytes each) ──────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} ext2_bgd_t;

/* ── On-disk inode (128 bytes classic) ───────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;   /* in 512-byte units */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_inode_t;

/* ── On-disk directory entry ─────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[256];
} ext2_dir_entry_t;

/* ── Filesystem instance private data ────────────────────────────────────── */
typedef struct {
    blkdev_t *dev;
    uint32_t  block_size;
    uint32_t  sectors_per_block;  /* block_size / 512               */
    uint32_t  blocks_per_group;
    uint32_t  inodes_per_group;
    uint32_t  inode_size;
    uint32_t  first_data_block;
    uint32_t  num_groups;
    uint32_t  total_inodes;
    uint32_t  total_blocks;
    uint32_t  bgd_block;          /* block number of first BGD      */
} ext2_sb_t;

/* ── Per-file private data ───────────────────────────────────────────────── */
typedef struct {
    uint32_t ino;
} ext2_node_t;

/* ── Low-level I/O helpers ───────────────────────────────────────────────── */

/* Convert ext2 block number → starting LBA (512-byte sector) */
static uint64_t blk_to_lba(ext2_sb_t *sb, uint32_t block) {
    return (uint64_t)block * sb->sectors_per_block;
}

/* Read `len` bytes from ext2 block `block` starting at byte `offset` into `buf`.
 * Works across multiple 512-byte cache buffers. */
static int ext2_read_block_partial(ext2_sb_t *sb, uint32_t block, uint32_t offset,
                                    void *buf, uint32_t len) {
    if (block == 0) { memset(buf, 0, len); return 0; } /* sparse block */
    uint8_t *dst = (uint8_t *)buf;
    while (len > 0) {
        uint32_t sec_off = offset / EXT2_SECTOR_SIZE;
        uint32_t byte_off = offset % EXT2_SECTOR_SIZE;
        uint32_t copy = EXT2_SECTOR_SIZE - byte_off;
        if (copy > len) copy = len;
        bcache_buf_t *b = bcache_get(sb->dev, blk_to_lba(sb, block) + sec_off);
        if (!b) return -EIO;
        memcpy(dst, b->data + byte_off, copy);
        bcache_release(b);
        dst += copy; offset += copy; len -= copy;
    }
    return 0;
}

static int ext2_write_block_partial(ext2_sb_t *sb, uint32_t block, uint32_t offset,
                                     const void *buf, uint32_t len) {
    uint8_t *src = (uint8_t *)buf;
    while (len > 0) {
        uint32_t sec_off  = offset / EXT2_SECTOR_SIZE;
        uint32_t byte_off = offset % EXT2_SECTOR_SIZE;
        uint32_t copy = EXT2_SECTOR_SIZE - byte_off;
        if (copy > len) copy = len;
        bcache_buf_t *b = bcache_get(sb->dev, blk_to_lba(sb, block) + sec_off);
        if (!b) return -EIO;
        memcpy(b->data + byte_off, src, copy);
        bcache_mark_dirty(b);
        bcache_release(b);
        src += copy; offset += copy; len -= copy;
    }
    return 0;
}

/* Allocate a temporary ext2 block-sized scratch buffer. */
static uint8_t *ext2_alloc_block_tmp(ext2_sb_t *sb) {
    if (!sb || sb->block_size == 0 || sb->block_size > 4096) return NULL;
    return (uint8_t *)kmalloc(sb->block_size);
}

/* Zero-fill a full ext2 block on disk without using large stack buffers. */
static int ext2_zero_block(ext2_sb_t *sb, uint32_t block) {
    uint8_t *z = ext2_alloc_block_tmp(sb);
    if (!z) return -ENOMEM;
    memset(z, 0, sb->block_size);
    int r = ext2_write_block_partial(sb, block, 0, z, sb->block_size);
    kfree(z);
    return r;
}

/* Read entire block into caller's buffer (must be >= block_size bytes).    */
/* Note: currently unused but provided for completeness; suppress warning.  */
__attribute__((unused))
static int ext2_read_block(ext2_sb_t *sb, uint32_t block, void *buf) {
    return ext2_read_block_partial(sb, block, 0, buf, sb->block_size);
}

/* ── Block Group Descriptor ──────────────────────────────────────────────── */
static int ext2_read_bgd(ext2_sb_t *sb, uint32_t group, ext2_bgd_t *out) {
    uint32_t offset = group * sizeof(ext2_bgd_t);
    uint32_t block  = sb->bgd_block + offset / sb->block_size;
    uint32_t boff   = offset % sb->block_size;
    return ext2_read_block_partial(sb, block, boff, out, sizeof(ext2_bgd_t));
}

static int ext2_write_bgd(ext2_sb_t *sb, uint32_t group, const ext2_bgd_t *bgd) {
    uint32_t offset = group * sizeof(ext2_bgd_t);
    uint32_t block  = sb->bgd_block + offset / sb->block_size;
    uint32_t boff   = offset % sb->block_size;
    return ext2_write_block_partial(sb, block, boff, bgd, sizeof(ext2_bgd_t));
}

/* ── Inode read/write ────────────────────────────────────────────────────── */
static int ext2_read_inode(ext2_sb_t *sb, uint32_t ino, ext2_inode_t *out) {
    uint32_t group      = (ino - 1) / sb->inodes_per_group;
    uint32_t idx        = (ino - 1) % sb->inodes_per_group;
    ext2_bgd_t bgd;
    if (ext2_read_bgd(sb, group, &bgd) < 0) return -EIO;

    uint32_t inode_block_offset = idx * sb->inode_size;
    uint32_t block   = bgd.bg_inode_table + inode_block_offset / sb->block_size;
    uint32_t boff    = inode_block_offset % sb->block_size;
    uint32_t read_sz = sb->inode_size < sizeof(ext2_inode_t) ? sb->inode_size
                                                              : sizeof(ext2_inode_t);
    return ext2_read_block_partial(sb, block, boff, out, read_sz);
}

static int ext2_write_inode(ext2_sb_t *sb, uint32_t ino, const ext2_inode_t *in) {
    uint32_t group = (ino - 1) / sb->inodes_per_group;
    uint32_t idx   = (ino - 1) % sb->inodes_per_group;
    ext2_bgd_t bgd;
    if (ext2_read_bgd(sb, group, &bgd) < 0) return -EIO;

    uint32_t off   = idx * sb->inode_size;
    uint32_t block = bgd.bg_inode_table + off / sb->block_size;
    uint32_t boff  = off % sb->block_size;
    uint32_t wsz   = sb->inode_size < sizeof(ext2_inode_t) ? sb->inode_size
                                                            : sizeof(ext2_inode_t);
    return ext2_write_block_partial(sb, block, boff, in, wsz);
}

/* ── Block allocation from group bitmap ──────────────────────────────────── */
static uint32_t ext2_alloc_block_in_group(ext2_sb_t *sb, uint32_t group) {
    ext2_bgd_t bgd;
    if (ext2_read_bgd(sb, group, &bgd) < 0) return 0;
    if (bgd.bg_free_blocks_count == 0) return 0;

    /* Read block bitmap */
    uint8_t *bitmap = ext2_alloc_block_tmp(sb);
    if (!bitmap) return 0;
    uint32_t bitmap_bytes = sb->blocks_per_group / 8;
    if (bitmap_bytes > sb->block_size) bitmap_bytes = sb->block_size;

    if (ext2_read_block_partial(sb, bgd.bg_block_bitmap, 0,
                                 bitmap, bitmap_bytes) < 0) {
        kfree(bitmap);
        return 0;
    }

    for (uint32_t i = 0; i < bitmap_bytes * 8; i++) {
        uint32_t byte = i / 8, bit = i % 8;
        if (!(bitmap[byte] & (1u << bit))) {
            bitmap[byte] |= (1u << bit);
            ext2_write_block_partial(sb, bgd.bg_block_bitmap, byte, &bitmap[byte], 1);
            bgd.bg_free_blocks_count--;
            ext2_write_bgd(sb, group, &bgd);
            kfree(bitmap);
            return group * sb->blocks_per_group + i + sb->first_data_block;
        }
    }
    kfree(bitmap);
    return 0;
}

static uint32_t ext2_alloc_block(ext2_sb_t *sb) {
    for (uint32_t g = 0; g < sb->num_groups; g++) {
        uint32_t blk = ext2_alloc_block_in_group(sb, g);
        if (blk) return blk;
    }
    return 0;
}

static void ext2_free_block(ext2_sb_t *sb, uint32_t blk) {
    if (blk < sb->first_data_block) return;
    uint32_t group = (blk - sb->first_data_block) / sb->blocks_per_group;
    uint32_t idx   = (blk - sb->first_data_block) % sb->blocks_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(sb, group, &bgd) < 0) return;

    uint8_t byte;
    ext2_read_block_partial(sb, bgd.bg_block_bitmap, idx / 8, &byte, 1);
    byte &= ~(1u << (idx % 8));
    ext2_write_block_partial(sb, bgd.bg_block_bitmap, idx / 8, &byte, 1);
    bgd.bg_free_blocks_count++;
    ext2_write_bgd(sb, group, &bgd);
}

/* ── Inode allocation ────────────────────────────────────────────────────── */
static uint32_t ext2_alloc_inode(ext2_sb_t *sb) {
    for (uint32_t g = 0; g < sb->num_groups; g++) {
        ext2_bgd_t bgd;
        if (ext2_read_bgd(sb, g, &bgd) < 0) continue;
        if (bgd.bg_free_inodes_count == 0) continue;

        uint8_t *bitmap = ext2_alloc_block_tmp(sb);
        if (!bitmap) continue;
        uint32_t bm_bytes = sb->inodes_per_group / 8;
        if (bm_bytes > sb->block_size) bm_bytes = sb->block_size;
        if (ext2_read_block_partial(sb, bgd.bg_inode_bitmap, 0, bitmap, bm_bytes) < 0) {
            kfree(bitmap);
            continue;
        }

        for (uint32_t i = 0; i < bm_bytes * 8; i++) {
            uint32_t byte = i / 8, bit = i % 8;
            if (!(bitmap[byte] & (1u << bit))) {
                bitmap[byte] |= (1u << bit);
                ext2_write_block_partial(sb, bgd.bg_inode_bitmap, byte, &bitmap[byte], 1);
                bgd.bg_free_inodes_count--;
                ext2_write_bgd(sb, g, &bgd);
                kfree(bitmap);
                return g * sb->inodes_per_group + i + 1; /* 1-based */
            }
        }
        kfree(bitmap);
    }
    return 0;
}

static void ext2_free_inode(ext2_sb_t *sb, uint32_t ino) {
    uint32_t group = (ino - 1) / sb->inodes_per_group;
    uint32_t idx   = (ino - 1) % sb->inodes_per_group;
    ext2_bgd_t bgd;
    if (ext2_read_bgd(sb, group, &bgd) < 0) return;
    uint8_t byte;
    ext2_read_block_partial(sb, bgd.bg_inode_bitmap, idx / 8, &byte, 1);
    byte &= ~(1u << (idx % 8));
    ext2_write_block_partial(sb, bgd.bg_inode_bitmap, idx / 8, &byte, 1);
    bgd.bg_free_inodes_count++;
    ext2_write_bgd(sb, group, &bgd);
}

static bool ext2_indirect_block_empty(ext2_sb_t *sb, uint32_t blk) {
    if (!blk) return true;
    uint8_t *tmp = ext2_alloc_block_tmp(sb);
    if (!tmp) return false;
    if (ext2_read_block_partial(sb, blk, 0, tmp, sb->block_size) < 0) {
        kfree(tmp);
        return false;
    }
    uint32_t *entries = (uint32_t *)tmp;
    uint32_t count = sb->block_size / 4;
    bool empty = true;
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i] != 0) { empty = false; break; }
    }
    kfree(tmp);
    return empty;
}

static void ext2_free_indirect_tree(ext2_sb_t *sb, uint32_t blk, int level) {
    if (!blk || level <= 0) return;

    uint8_t *tmp = ext2_alloc_block_tmp(sb);
    if (!tmp) return;
    if (ext2_read_block_partial(sb, blk, 0, tmp, sb->block_size) < 0) {
        kfree(tmp);
        return;
    }

    uint32_t *entries = (uint32_t *)tmp;
    uint32_t count = sb->block_size / 4;
    for (uint32_t i = 0; i < count; i++) {
        if (!entries[i]) continue;
        if (level == 1) ext2_free_block(sb, entries[i]);
        else ext2_free_indirect_tree(sb, entries[i], level - 1);
    }
    kfree(tmp);
    ext2_free_block(sb, blk);
}

static void ext2_inode_free_all_blocks(ext2_sb_t *sb, ext2_inode_t *inode) {
    for (int i = 0; i < 12; i++) {
        if (inode->i_block[i]) {
            ext2_free_block(sb, inode->i_block[i]);
            inode->i_block[i] = 0;
        }
    }
    if (inode->i_block[12]) {
        ext2_free_indirect_tree(sb, inode->i_block[12], 1);
        inode->i_block[12] = 0;
    }
    if (inode->i_block[13]) {
        ext2_free_indirect_tree(sb, inode->i_block[13], 2);
        inode->i_block[13] = 0;
    }
    if (inode->i_block[14]) {
        ext2_free_indirect_tree(sb, inode->i_block[14], 3);
        inode->i_block[14] = 0;
    }
    inode->i_blocks = 0;
}

/* ── Block mapping (logical → physical) ──────────────────────────────────── */
/* Number of 32-bit block pointers per indirect block */
#define PTR_PER_BLK(sb) ((sb)->block_size / 4)

/* Resolve logical block index within inode to physical block number.
 * Returns 0 for sparse blocks, negative errno on error. */
static int32_t ext2_inode_get_block(ext2_sb_t *sb, ext2_inode_t *inode,
                                     uint32_t log_blk) {
    uint32_t ppb = PTR_PER_BLK(sb);

    if (log_blk < 12) {
        return (int32_t)inode->i_block[log_blk];
    }
    log_blk -= 12;

    if (log_blk < ppb) {
        /* Single indirect */
        if (!inode->i_block[12]) return 0;
        uint32_t entry;
        if (ext2_read_block_partial(sb, inode->i_block[12], log_blk * 4, &entry, 4) < 0)
            return -EIO;
        return (int32_t)entry;
    }
    log_blk -= ppb;

    if (log_blk < ppb * ppb) {
        /* Double indirect */
        if (!inode->i_block[13]) return 0;
        uint32_t idx1 = log_blk / ppb, idx2 = log_blk % ppb;
        uint32_t blk1, blk2;
        if (ext2_read_block_partial(sb, inode->i_block[13], idx1 * 4, &blk1, 4) < 0) return -EIO;
        if (!blk1) return 0;
        if (ext2_read_block_partial(sb, blk1, idx2 * 4, &blk2, 4) < 0) return -EIO;
        return (int32_t)blk2;
    }
    log_blk -= ppb * ppb;

    /* Triple indirect */
    if (!inode->i_block[14]) return 0;
    uint32_t idx1 = log_blk / (ppb * ppb);
    uint32_t idx2 = (log_blk / ppb) % ppb;
    uint32_t idx3 = log_blk % ppb;
    uint32_t b1, b2, b3;
    if (ext2_read_block_partial(sb, inode->i_block[14], idx1 * 4, &b1, 4) < 0) return -EIO;
    if (!b1) return 0;
    if (ext2_read_block_partial(sb, b1, idx2 * 4, &b2, 4) < 0) return -EIO;
    if (!b2) return 0;
    if (ext2_read_block_partial(sb, b2, idx3 * 4, &b3, 4) < 0) return -EIO;
    return (int32_t)b3;
}

/* Set logical block index in inode, allocating indirect blocks as needed. */
static int ext2_inode_set_block(ext2_sb_t *sb, ext2_inode_t *inode,
                                  uint32_t log_blk, uint32_t phys_blk) {
    uint32_t ppb = PTR_PER_BLK(sb);

    if (log_blk < 12) {
        inode->i_block[log_blk] = phys_blk;
        return 0;
    }
    log_blk -= 12;

    if (log_blk < ppb) {
        if (!inode->i_block[12]) {
            inode->i_block[12] = ext2_alloc_block(sb);
            if (!inode->i_block[12]) return -ENOSPC;
            if (ext2_zero_block(sb, inode->i_block[12]) < 0) return -EIO;
        }
        return ext2_write_block_partial(sb, inode->i_block[12], log_blk * 4, &phys_blk, 4);
    }
    log_blk -= ppb;

    if (log_blk < ppb * ppb) {
        if (!inode->i_block[13]) {
            inode->i_block[13] = ext2_alloc_block(sb);
            if (!inode->i_block[13]) return -ENOSPC;
            if (ext2_zero_block(sb, inode->i_block[13]) < 0) return -EIO;
        }
        uint32_t idx1 = log_blk / ppb, idx2 = log_blk % ppb, b1;
        ext2_read_block_partial(sb, inode->i_block[13], idx1 * 4, &b1, 4);
        if (!b1) {
            b1 = ext2_alloc_block(sb);
            if (!b1) return -ENOSPC;
            if (ext2_zero_block(sb, b1) < 0) return -EIO;
            ext2_write_block_partial(sb, inode->i_block[13], idx1 * 4, &b1, 4);
        }
        return ext2_write_block_partial(sb, b1, idx2 * 4, &phys_blk, 4);
    }
    return -ENOSPC; /* triple indirect not needed for typical files */
}

static uint32_t ext2_inode_clear_block(ext2_sb_t *sb, ext2_inode_t *inode,
                                       uint32_t log_blk) {
    uint32_t ppb = PTR_PER_BLK(sb);

    if (log_blk < 12) {
        uint32_t old = inode->i_block[log_blk];
        inode->i_block[log_blk] = 0;
        return old;
    }
    log_blk -= 12;

    if (log_blk < ppb) {
        if (!inode->i_block[12]) return 0;
        uint32_t zero = 0, old = 0;
        if (ext2_read_block_partial(sb, inode->i_block[12], log_blk * 4, &old, 4) < 0)
            return 0;
        if (!old) return 0;
        ext2_write_block_partial(sb, inode->i_block[12], log_blk * 4, &zero, 4);
        if (ext2_indirect_block_empty(sb, inode->i_block[12])) {
            ext2_free_block(sb, inode->i_block[12]);
            inode->i_block[12] = 0;
        }
        return old;
    }
    log_blk -= ppb;

    if (log_blk < ppb * ppb) {
        if (!inode->i_block[13]) return 0;
        uint32_t idx1 = log_blk / ppb;
        uint32_t idx2 = log_blk % ppb;
        uint32_t b1 = 0, old = 0, zero = 0;
        if (ext2_read_block_partial(sb, inode->i_block[13], idx1 * 4, &b1, 4) < 0 || !b1)
            return 0;
        if (ext2_read_block_partial(sb, b1, idx2 * 4, &old, 4) < 0 || !old)
            return 0;
        ext2_write_block_partial(sb, b1, idx2 * 4, &zero, 4);
        if (ext2_indirect_block_empty(sb, b1)) {
            ext2_free_block(sb, b1);
            ext2_write_block_partial(sb, inode->i_block[13], idx1 * 4, &zero, 4);
        }
        if (inode->i_block[13] && ext2_indirect_block_empty(sb, inode->i_block[13])) {
            ext2_free_block(sb, inode->i_block[13]);
            inode->i_block[13] = 0;
        }
        return old;
    }
    log_blk -= ppb * ppb;

    if (!inode->i_block[14]) return 0;
    uint32_t idx1 = log_blk / (ppb * ppb);
    uint32_t idx2 = (log_blk / ppb) % ppb;
    uint32_t idx3 = log_blk % ppb;
    uint32_t b1 = 0, b2 = 0, old = 0, zero = 0;

    if (ext2_read_block_partial(sb, inode->i_block[14], idx1 * 4, &b1, 4) < 0 || !b1)
        return 0;
    if (ext2_read_block_partial(sb, b1, idx2 * 4, &b2, 4) < 0 || !b2)
        return 0;
    if (ext2_read_block_partial(sb, b2, idx3 * 4, &old, 4) < 0 || !old)
        return 0;

    ext2_write_block_partial(sb, b2, idx3 * 4, &zero, 4);
    if (ext2_indirect_block_empty(sb, b2)) {
        ext2_free_block(sb, b2);
        ext2_write_block_partial(sb, b1, idx2 * 4, &zero, 4);
    }
    if (ext2_indirect_block_empty(sb, b1)) {
        ext2_free_block(sb, b1);
        ext2_write_block_partial(sb, inode->i_block[14], idx1 * 4, &zero, 4);
    }
    if (inode->i_block[14] && ext2_indirect_block_empty(sb, inode->i_block[14])) {
        ext2_free_block(sb, inode->i_block[14]);
        inode->i_block[14] = 0;
    }
    return old;
}

/* ── Get superblock from fsi ─────────────────────────────────────────────── */
static inline ext2_sb_t *get_sb(vnode_t *v) { return (ext2_sb_t *)v->fsi->priv; }
static inline uint32_t   get_ino(vnode_t *v) { return ((ext2_node_t*)v->fs_data)->ino; }

static int ext2_chmod(vnode_t *v, uint32_t mode) {
    ext2_sb_t *sb = get_sb(v);
    ext2_inode_t inode;
    if (ext2_read_inode(sb, get_ino(v), &inode) < 0) return -EIO;
    inode.i_mode = (uint16_t)((inode.i_mode & EXT2_IFMT) | (mode & 0x0FFF));
    if (ext2_write_inode(sb, get_ino(v), &inode) < 0) return -EIO;
    v->mode = (v->mode & VFS_S_IFMT) | (mode & 07777);
    return 0;
}

static int ext2_chown(vnode_t *v, int owner, int group) {
    ext2_sb_t *sb = get_sb(v);
    ext2_inode_t inode;
    if (ext2_read_inode(sb, get_ino(v), &inode) < 0) return -EIO;
    if (owner >= 0) inode.i_uid = (uint16_t)owner;
    if (group >= 0) inode.i_gid = (uint16_t)group;
    if (ext2_write_inode(sb, get_ino(v), &inode) < 0) return -EIO;
    if (owner >= 0) v->uid = (uint32_t)owner;
    if (group >= 0) v->gid = (uint32_t)group;
    return 0;
}

/* ── Build vnode from inode number ────────────────────────────────────────── */
static vnode_t *make_vnode(ext2_sb_t *sb, fs_inst_t *fsi, uint32_t ino,
                            ext2_inode_t *inode) {
    vnode_t      *v  = vfs_alloc_vnode();
    ext2_node_t  *nd = kzalloc(sizeof(ext2_node_t));
    if (!v || !nd) { kfree(v); kfree(nd); return NULL; }
    nd->ino = ino;

    uint32_t ifmt = inode->i_mode & EXT2_IFMT;
    uint32_t mode_bits = inode->i_mode & 0xFFF;
    uint32_t vmode;
    if      (ifmt == EXT2_S_IFREG)  vmode = VFS_S_IFREG | mode_bits;
    else if (ifmt == EXT2_S_IFDIR)  vmode = VFS_S_IFDIR | mode_bits;
    else if (ifmt == EXT2_S_IFLNK)  vmode = VFS_S_IFLNK | mode_bits;
    else                             vmode = mode_bits;

    v->ino      = ino;
    v->mode     = vmode;
    v->size     = inode->i_size;
    v->uid      = inode->i_uid;
    v->gid      = inode->i_gid;
    v->nlink    = inode->i_links_count;
    v->atime    = inode->i_atime;
    v->mtime    = inode->i_mtime;
    v->ctime    = inode->i_ctime;
    v->ops      = &g_ext2_ops;
    v->fsi      = fsi;
    v->fs_data  = nd;
    v->refcount = 1;
    return v;
}

/* ── fs_ops implementations ──────────────────────────────────────────────── */
static vnode_t *ext2_lookup(vnode_t *dir, const char *name) {
    ext2_sb_t   *sb  = get_sb(dir);
    ext2_inode_t inode, child_inode;
    if (ext2_read_inode(sb, get_ino(dir), &inode) < 0) return NULL;
    uint8_t *buf = ext2_alloc_block_tmp(sb);
    if (!buf) return NULL;

    uint64_t dir_size = inode.i_size;
    uint64_t off = 0;

    while (off < dir_size) {
        uint32_t log_blk = (uint32_t)(off / sb->block_size);
        uint32_t blk_off = (uint32_t)(off % sb->block_size);

        int32_t phys = ext2_inode_get_block(sb, &inode, log_blk);
        if (phys <= 0) { off += sb->block_size - blk_off; continue; }

        /* Read directory entries from this block up to its end */
        uint32_t remain = sb->block_size - blk_off;
        uint32_t rd = remain < sb->block_size ? remain : sb->block_size;
        if (ext2_read_block_partial(sb, (uint32_t)phys, blk_off, buf, rd) < 0) break;

        uint32_t boff = 0;
        while (boff + sizeof(uint32_t) + sizeof(uint16_t) + 2 <= rd) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(buf + boff);
            if (de->rec_len < 8) break;
            if (de->inode != 0) {
                char n[256];
                uint32_t nl = de->name_len < 255 ? de->name_len : 255;
                memcpy(n, de->name, nl);
                n[nl] = '\0';
                if (strcmp(n, name) == 0) {
                    if (ext2_read_inode(sb, de->inode, &child_inode) < 0) {
                        kfree(buf);
                        return NULL;
                    }
                    vnode_t *found = make_vnode(sb, dir->fsi, de->inode, &child_inode);
                    kfree(buf);
                    return found;
                }
            }
            boff += de->rec_len;
        }
        off += rd;
    }
    kfree(buf);
    return NULL;
}

static int ext2_open(vnode_t *v, int flags)  { (void)v; (void)flags; return 0; }
static int ext2_close(vnode_t *v)            { (void)v; return 0; }

static ssize_t ext2_read(vnode_t *v, void *buf, size_t len, uint64_t off) {
    ext2_sb_t   *sb = get_sb(v);
    ext2_inode_t inode;
    if (ext2_read_inode(sb, get_ino(v), &inode) < 0) return -EIO;

    if (off >= inode.i_size) return 0;
    if (off + len > inode.i_size) len = (size_t)(inode.i_size - off);

    uint8_t *dst  = (uint8_t *)buf;
    size_t   done = 0;

    while (done < len) {
        uint32_t log_blk = (uint32_t)((off + done) / sb->block_size);
        uint32_t blk_off = (uint32_t)((off + done) % sb->block_size);
        uint32_t copy    = sb->block_size - blk_off;
        if (copy > len - done) copy = (uint32_t)(len - done);

        int32_t phys = ext2_inode_get_block(sb, &inode, log_blk);
        if (phys < 0) return done == 0 ? -EIO : (ssize_t)done;
        if (phys == 0) { memset(dst + done, 0, copy); }
        else {
            if (ext2_read_block_partial(sb, (uint32_t)phys, blk_off, dst + done, copy) < 0)
                return done == 0 ? -EIO : (ssize_t)done;
        }
        done += copy;
    }
    return (ssize_t)done;
}

static ssize_t ext2_write(vnode_t *v, const void *buf, size_t len, uint64_t off) {
    if (len == 0) return 0;
    ext2_sb_t   *sb = get_sb(v);
    ext2_inode_t inode;
    if (ext2_read_inode(sb, get_ino(v), &inode) < 0) return -EIO;

    const uint8_t *src  = (const uint8_t *)buf;
    size_t         done = 0;

    while (done < len) {
        uint32_t log_blk = (uint32_t)((off + done) / sb->block_size);
        uint32_t blk_off = (uint32_t)((off + done) % sb->block_size);
        uint32_t copy    = sb->block_size - blk_off;
        if (copy > len - done) copy = (uint32_t)(len - done);

        int32_t phys = ext2_inode_get_block(sb, &inode, log_blk);
        if (phys < 0) return done == 0 ? phys : (ssize_t)done;
        if (phys == 0) {
            /* Allocate a new block */
            uint32_t nb = ext2_alloc_block(sb);
            if (!nb) return done == 0 ? -ENOSPC : (ssize_t)done;
            if (ext2_inode_set_block(sb, &inode, log_blk, nb) < 0)
                return done == 0 ? -ENOSPC : (ssize_t)done;
            inode.i_blocks += sb->sectors_per_block;
            phys = (int32_t)nb;
        }
        if (ext2_write_block_partial(sb, (uint32_t)phys, blk_off, src + done, copy) < 0)
            return done == 0 ? -EIO : (ssize_t)done;
        done += copy;
    }

    if (off + done > inode.i_size) inode.i_size = (uint32_t)(off + done);
    v->size = inode.i_size;
    ext2_write_inode(sb, get_ino(v), &inode);
    return (ssize_t)done;
}

static int ext2_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out) {
    ext2_sb_t   *sb = get_sb(dir);
    ext2_inode_t inode;
    if (ext2_read_inode(sb, get_ino(dir), &inode) < 0) return -EIO;
    uint8_t *blkbuf = ext2_alloc_block_tmp(sb);
    if (!blkbuf) return -ENOMEM;

    uint64_t off = *cookie;
    while (off < inode.i_size) {
        uint32_t log_blk = (uint32_t)(off / sb->block_size);
        uint32_t blk_off = (uint32_t)(off % sb->block_size);

        int32_t phys = ext2_inode_get_block(sb, &inode, log_blk);
        if (phys <= 0) { off = (uint64_t)(log_blk + 1) * sb->block_size; *cookie=off; continue; }

        uint32_t rd = sb->block_size;
        if (ext2_read_block_partial(sb, (uint32_t)phys, 0, blkbuf, rd) < 0) {
            kfree(blkbuf);
            return -EIO;
        }

        while (blk_off + 8 <= rd) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(blkbuf + blk_off);
            if (de->rec_len < 8) break;
            if (de->inode != 0) {
                out->ino = de->inode;
                uint32_t nl = de->name_len < VFS_NAME_MAX ? de->name_len : VFS_NAME_MAX;
                memcpy(out->name, de->name, nl);
                out->name[nl] = '\0';
                out->type = (de->file_type == EXT2_DT_DIR) ? VFS_DT_DIR :
                            (de->file_type == EXT2_DT_LNK) ? VFS_DT_LNK : VFS_DT_REG;
                *cookie = (uint64_t)(log_blk * sb->block_size + blk_off + de->rec_len);
                kfree(blkbuf);
                return 1;
            }
            blk_off += de->rec_len;
        }
        /* Advance to next block */
        off = (uint64_t)(log_blk + 1) * sb->block_size;
    }
    kfree(blkbuf);
    return 0; /* EOF */
}

/* ── Directory entry helpers ─────────────────────────────────────────────── */
/* Append a directory entry to `dir_ino`. Returns 0 on success. */
static int ext2_dir_add_entry(ext2_sb_t *sb, uint32_t dir_ino, uint32_t child_ino,
                               const char *name, uint8_t file_type) {
    ext2_inode_t inode;
    if (ext2_read_inode(sb, dir_ino, &inode) < 0) return -EIO;
    uint8_t *blk = ext2_alloc_block_tmp(sb);
    if (!blk) return -ENOMEM;

    uint8_t name_len = (uint8_t)strlen(name);
    uint16_t needed  = (uint16_t)((8 + name_len + 3) & ~3u); /* 4-byte aligned */

    /* Search for space in existing blocks */
    for (uint32_t lb = 0; lb < (inode.i_size + sb->block_size - 1) / sb->block_size; lb++) {
        int32_t phys = ext2_inode_get_block(sb, &inode, lb);
        if (phys <= 0) continue;

        uint32_t bsz = sb->block_size;
        if (ext2_read_block_partial(sb, (uint32_t)phys, 0, blk, bsz) < 0) continue;

        uint32_t off = 0;
        while (off + 8 <= bsz) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(blk + off);
            if (de->rec_len < 8) break;
            /* Compute actual size of this entry */
            uint16_t actual = (uint16_t)((8 + de->name_len + 3) & ~3u);
            uint16_t slack  = de->rec_len - actual;
            if (slack >= needed) {
                /* Split: shrink current, add new after */
                de->rec_len = actual;
                ext2_dir_entry_t *ne = (ext2_dir_entry_t *)(blk + off + actual);
                ne->inode     = child_ino;
                ne->rec_len   = slack;
                ne->name_len  = name_len;
                ne->file_type = file_type;
                memcpy(ne->name, name, name_len);
                ext2_write_block_partial(sb, (uint32_t)phys, 0, blk, bsz);
                kfree(blk);
                return 0;
            }
            off += de->rec_len;
        }
    }

    /* No space — allocate new block */
    uint32_t new_lb = (inode.i_size + sb->block_size - 1) / sb->block_size;
    uint32_t nb     = ext2_alloc_block(sb);
    if (!nb) { kfree(blk); return -ENOSPC; }
    if (ext2_inode_set_block(sb, &inode, new_lb, nb) < 0) { kfree(blk); return -ENOSPC; }
    inode.i_size   += sb->block_size;
    inode.i_blocks += sb->sectors_per_block;

    /* Write single entry that spans entire block */
    uint32_t bsz = sb->block_size;
    memset(blk, 0, bsz);
    ext2_dir_entry_t *ne = (ext2_dir_entry_t *)blk;
    ne->inode     = child_ino;
    ne->rec_len   = (uint16_t)bsz;
    ne->name_len  = name_len;
    ne->file_type = file_type;
    memcpy(ne->name, name, name_len);
    ext2_write_block_partial(sb, nb, 0, blk, bsz);
    int wr = ext2_write_inode(sb, dir_ino, &inode);
    kfree(blk);
    return wr;
}

static vnode_t *ext2_create(vnode_t *parent, const char *name, uint32_t mode) {
    ext2_sb_t *sb = get_sb(parent);

    uint32_t ino = ext2_alloc_inode(sb);
    if (!ino) return NULL;

    ext2_inode_t inode = {0};
    inode.i_mode = EXT2_S_IFREG | (mode & 0xFFF);
    inode.i_links_count = 1;
    if (ext2_write_inode(sb, ino, &inode) < 0) { ext2_free_inode(sb, ino); return NULL; }

    if (ext2_dir_add_entry(sb, get_ino(parent), ino, name, EXT2_DT_REG) < 0) {
        ext2_free_inode(sb, ino);
        return NULL;
    }
    return make_vnode(sb, parent->fsi, ino, &inode);
}

static vnode_t *ext2_mkdir_op(vnode_t *parent, const char *name, uint32_t mode) {
    ext2_sb_t *sb = get_sb(parent);

    uint32_t ino = ext2_alloc_inode(sb);
    if (!ino) return NULL;

    ext2_inode_t inode = {0};
    inode.i_mode = EXT2_S_IFDIR | (mode & 0xFFF);
    inode.i_links_count = 2; /* "." + parent's ".." */
    inode.i_size = sb->block_size;
    uint32_t nb = ext2_alloc_block(sb);
    if (!nb) { ext2_free_inode(sb, ino); return NULL; }
    inode.i_blocks = sb->sectors_per_block;
    inode.i_block[0] = nb;

    /* Populate "." and ".." in the new block */
    uint8_t *blk = ext2_alloc_block_tmp(sb);
    if (!blk) { ext2_free_inode(sb, ino); return NULL; }
    uint32_t bsz = sb->block_size;
    memset(blk, 0, bsz);
    ext2_dir_entry_t *dot    = (ext2_dir_entry_t *)blk;
    dot->inode = ino; dot->name_len = 1; dot->file_type = EXT2_DT_DIR;
    memcpy(dot->name, ".", 1); dot->rec_len = 12;

    ext2_dir_entry_t *dotdot = (ext2_dir_entry_t *)(blk + 12);
    dotdot->inode = get_ino(parent); dotdot->name_len = 2; dotdot->file_type = EXT2_DT_DIR;
    memcpy(dotdot->name, "..", 2); dotdot->rec_len = (uint16_t)(bsz - 12);

    ext2_write_block_partial(sb, nb, 0, blk, bsz);
    kfree(blk);
    if (ext2_write_inode(sb, ino, &inode) < 0) { ext2_free_inode(sb, ino); return NULL; }

    /* Update parent link count (for "..") */
    ext2_inode_t parent_inode;
    if (ext2_read_inode(sb, get_ino(parent), &parent_inode) == 0) {
        parent_inode.i_links_count++;
        ext2_write_inode(sb, get_ino(parent), &parent_inode);
    }

    ext2_dir_add_entry(sb, get_ino(parent), ino, name, EXT2_DT_DIR);
    return make_vnode(sb, parent->fsi, ino, &inode);
}

static int ext2_unlink(vnode_t *parent, const char *name) {
    ext2_sb_t   *sb = get_sb(parent);
    ext2_inode_t inode;
    if (ext2_read_inode(sb, get_ino(parent), &inode) < 0) return -EIO;
    uint8_t *blk = ext2_alloc_block_tmp(sb);
    if (!blk) return -ENOMEM;

    uint32_t dir_size = inode.i_size;
    for (uint32_t lb = 0; lb < (dir_size + sb->block_size - 1) / sb->block_size; lb++) {
        int32_t phys = ext2_inode_get_block(sb, &inode, lb);
        if (phys <= 0) continue;

        uint32_t bsz = sb->block_size;
        if (ext2_read_block_partial(sb, (uint32_t)phys, 0, blk, bsz) < 0) continue;

        uint32_t off = 0;
        ext2_dir_entry_t *prev = NULL;
        while (off + 8 <= bsz) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(blk + off);
            if (de->rec_len < 8) break;
            if (de->inode) {
                char n[257];
                uint32_t nl = de->name_len;  /* name_len is uint8_t, max 255 */
                                if (nl > 256) nl = 256;
                memcpy(n, de->name, nl); n[nl] = '\0';
                if (strcmp(n, name) == 0) {
                    uint32_t victim_ino = de->inode;
                    if (prev) prev->rec_len += de->rec_len;
                    else de->inode = 0;
                    ext2_write_block_partial(sb, (uint32_t)phys, 0, blk, bsz);
                    /* Decrement link count */
                    ext2_inode_t vi;
                    if (ext2_read_inode(sb, victim_ino, &vi) == 0) {
                        if (vi.i_links_count > 0) vi.i_links_count--;
                        if (vi.i_links_count == 0) {
                            ext2_inode_free_all_blocks(sb, &vi);
                            vi.i_size = 0;
                            ext2_free_inode(sb, victim_ino);
                        } else {
                            ext2_write_inode(sb, victim_ino, &vi);
                        }
                    }
                    kfree(blk);
                    return 0;
                }
            }
            prev = de;
            off += de->rec_len;
        }
    }
    kfree(blk);
    return -ENOENT;
}

static int ext2_rmdir(vnode_t *parent, const char *name) {
    return ext2_unlink(parent, name);
}

static int ext2_rename(vnode_t *old_parent, const char *old_name,
                        vnode_t *new_parent, const char *new_name) {
    ext2_sb_t *sb = get_sb(old_parent);
    vnode_t *target = ext2_lookup(old_parent, old_name);
    if (!target) return -ENOENT;

    uint32_t ino = get_ino(target);
    ext2_inode_t inode;
    int rc = ext2_read_inode(sb, ino, &inode);
    if (rc < 0) {
        target->ops->evict(target);
        kfree(target);
        return -EIO;
    }
    uint8_t ftype = (inode.i_mode & EXT2_IFMT) == EXT2_S_IFDIR ? EXT2_DT_DIR : EXT2_DT_REG;

    vnode_t *existing = ext2_lookup(new_parent, new_name);
    if (existing) {
        uint32_t dst_ino = get_ino(existing);
        existing->ops->evict(existing);
        kfree(existing);

        /* POSIX: rename over another hard-link to the same inode is a no-op. */
        if (dst_ino == ino) {
            target->ops->evict(target);
            kfree(target);
            return 0;
        }

        rc = ext2_unlink(new_parent, new_name);
        if (rc < 0) {
            target->ops->evict(target);
            kfree(target);
            return rc;
        }
    }

    /* Keep source inode alive while we do add-then-remove. */
    inode.i_links_count++;
    if (ext2_write_inode(sb, ino, &inode) < 0) {
        target->ops->evict(target);
        kfree(target);
        return -EIO;
    }

    rc = ext2_dir_add_entry(sb, get_ino(new_parent), ino, new_name, ftype);
    if (rc < 0) {
        if (ext2_read_inode(sb, ino, &inode) == 0 && inode.i_links_count > 0) {
            inode.i_links_count--;
            ext2_write_inode(sb, ino, &inode);
        }
        target->ops->evict(target);
        kfree(target);
        return rc;
    }

    rc = ext2_unlink(old_parent, old_name);
    if (rc < 0) {
        int rollback = ext2_unlink(new_parent, new_name);
        if (rollback < 0 && ext2_read_inode(sb, ino, &inode) == 0 && inode.i_links_count > 0) {
            inode.i_links_count--;
            ext2_write_inode(sb, ino, &inode);
        }
        target->ops->evict(target);
        kfree(target);
        return rc;
    }

    target->ops->evict(target);
    kfree(target);
    return 0;
}

static int ext2_stat(vnode_t *v, vfs_stat_t *st) {
    memset(st, 0, sizeof(*st));
    st->ino   = v->ino;
    st->mode  = v->mode;
    st->size  = v->size;
    st->nlink = v->nlink;
    st->uid   = v->uid;
    st->gid   = v->gid;
    st->atime = v->atime;
    st->mtime = v->mtime;
    st->ctime = v->ctime;
    return 0;
}

static vnode_t *ext2_symlink(vnode_t *parent, const char *name, const char *target) {
    ext2_sb_t *sb = get_sb(parent);
    if (!target || !name || !name[0]) return NULL;

    /* Fast symlink only (stored directly in i_block[]). */
    size_t tlen = strlen(target);
    if (tlen >= 60) return NULL;

    vnode_t *existing = ext2_lookup(parent, name);
    if (existing) {
        existing->ops->evict(existing);
        kfree(existing);
        return NULL;
    }

    uint32_t ino = ext2_alloc_inode(sb);
    if (!ino) return NULL;

    ext2_inode_t inode = {0};
    inode.i_mode = EXT2_S_IFLNK | 0777;
    inode.i_links_count = 1;
    inode.i_size = (uint32_t)tlen;
    memcpy(inode.i_block, target, tlen);

    if (ext2_write_inode(sb, ino, &inode) < 0) {
        ext2_free_inode(sb, ino);
        return NULL;
    }

    if (ext2_dir_add_entry(sb, get_ino(parent), ino, name, EXT2_DT_LNK) < 0) {
        ext2_free_inode(sb, ino);
        return NULL;
    }

    return make_vnode(sb, parent->fsi, ino, &inode);
}

static int ext2_readlink(vnode_t *v, char *buf, size_t sz) {
    ext2_sb_t   *sb = get_sb(v);
    ext2_inode_t inode;
    if (ext2_read_inode(sb, get_ino(v), &inode) < 0) return -EIO;
    /* Fast symlinks (< 60 bytes) stored directly in i_block[] */
    if (inode.i_size < 60) {
        size_t len = inode.i_size < sz - 1 ? inode.i_size : sz - 1;
        memcpy(buf, inode.i_block, len);
        buf[len] = '\0';
        return (int)len;
    }
    ssize_t r = ext2_read(v, buf, sz - 1, 0);
    if (r < 0) return (int)r;
    buf[r] = '\0';
    return (int)r;
}

static int ext2_truncate(vnode_t *v, uint64_t size) {
    ext2_sb_t   *sb = get_sb(v);
    ext2_inode_t inode;
    if (ext2_read_inode(sb, get_ino(v), &inode) < 0) return -EIO;

    if (size < inode.i_size) {
        if (size == 0) {
            ext2_inode_free_all_blocks(sb, &inode);
        } else {
            uint32_t old_blocks = (inode.i_size + sb->block_size - 1) / sb->block_size;
            uint32_t new_blocks = ((uint32_t)size + sb->block_size - 1) / sb->block_size;
            for (uint32_t lb = new_blocks; lb < old_blocks; lb++) {
                uint32_t old = ext2_inode_clear_block(sb, &inode, lb);
                if (old) {
                    ext2_free_block(sb, old);
                    if (inode.i_blocks >= sb->sectors_per_block)
                        inode.i_blocks -= sb->sectors_per_block;
                    else
                        inode.i_blocks = 0;
                }
            }
        }
    }

    inode.i_size = (uint32_t)size;
    v->size = inode.i_size;
    return ext2_write_inode(sb, get_ino(v), &inode);
}

static int ext2_sync_v(vnode_t *v) { (void)v; return 0; }

static void ext2_evict(vnode_t *v) {
    if (v->fs_data) { kfree(v->fs_data); v->fs_data = NULL; }
}

/* ── Mount ───────────────────────────────────────────────────────────────── */
static vnode_t *ext2_mount(fs_inst_t *fsi, blkdev_t *dev) {
    /* Superblock is at byte offset 1024 (= LBA 2, LBA 3 — two 512-B sectors).
     * Read the full 1024-byte on-disk superblock into a stack buffer, then
     * overlay the ext2_superblock_t struct pointer on top of it.  The struct
     * only describes the leading ~216 bytes we care about; the rest of the
     * on-disk superblock is silently discarded. */
    uint8_t sb_buf[1024];
    bcache_buf_t *b0 = bcache_get(dev, 2);
    bcache_buf_t *b1 = bcache_get(dev, 3);
    if (!b0 || !b1) { if (b0) bcache_release(b0); if (b1) bcache_release(b1); return NULL; }
    memcpy(sb_buf,       b0->data, 512);
    memcpy(sb_buf + 512, b1->data, 512);
    bcache_release(b0);
    bcache_release(b1);

    ext2_superblock_t *raw_sb = (ext2_superblock_t *)sb_buf;

    if (raw_sb->s_magic != EXT2_MAGIC) {
        KLOG_WARN("ext2: invalid magic on '%s'\n", dev->name);
        return NULL;
    }

    ext2_sb_t *sb = kzalloc(sizeof(ext2_sb_t));
    if (!sb) return NULL;

    sb->dev              = dev;
    sb->block_size       = 1024u << raw_sb->s_log_block_size;
    sb->sectors_per_block = sb->block_size / EXT2_SECTOR_SIZE;
    sb->blocks_per_group = raw_sb->s_blocks_per_group;
    sb->inodes_per_group = raw_sb->s_inodes_per_group;
    sb->inode_size       = raw_sb->s_rev_level >= 1 ? raw_sb->s_inode_size : 128;
    sb->first_data_block = raw_sb->s_first_data_block;
    sb->total_inodes     = raw_sb->s_inodes_count;
    sb->total_blocks     = raw_sb->s_blocks_count;
    sb->num_groups       = (raw_sb->s_blocks_count + raw_sb->s_blocks_per_group - 1)
                           / raw_sb->s_blocks_per_group;
    sb->bgd_block        = raw_sb->s_first_data_block + 1;

    fsi->priv = sb;

    KLOG_INFO("ext2: '%s' block_size=%u groups=%u inodes=%u\n",
              dev->name, sb->block_size, sb->num_groups, sb->total_inodes);

    ext2_inode_t root_inode;
    if (ext2_read_inode(sb, EXT2_ROOT_INO, &root_inode) < 0) {
        kfree(sb); return NULL;
    }
    return make_vnode(sb, fsi, EXT2_ROOT_INO, &root_inode);
}

static void ext2_unmount(fs_inst_t *fsi) {
    bcache_flush_dev(((ext2_sb_t*)fsi->priv)->dev);
    kfree(fsi->priv);
    fsi->priv = NULL;
}

/* ── fs_ops vtable ───────────────────────────────────────────────────────── */
fs_ops_t g_ext2_ops = {
    .name     = "ext2",
    .lookup   = ext2_lookup,
    .open     = ext2_open,
    .close    = ext2_close,
    .read     = ext2_read,
    .write    = ext2_write,
    .readdir  = ext2_readdir,
    .create   = ext2_create,
    .mkdir    = ext2_mkdir_op,
    .unlink   = ext2_unlink,
    .rmdir    = ext2_rmdir,
    .rename   = ext2_rename,
    .stat     = ext2_stat,
    .symlink  = ext2_symlink,
    .readlink = ext2_readlink,
    .truncate = ext2_truncate,
    .chmod    = ext2_chmod,
    .chown    = ext2_chown,
    .sync     = ext2_sync_v,
    .evict    = ext2_evict,
    .mount    = ext2_mount,
    .unmount  = ext2_unmount,
};

void ext2_register(void) {
    vfs_register_fs(&g_ext2_ops);
}
