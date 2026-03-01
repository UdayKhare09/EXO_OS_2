/* fs/fat32/fat32.c — FAT32 read/write filesystem driver
 *
 * Supports: BPB parsing, FAT chain traversal, LFN directory entries,
 *           file read/write (with cluster allocation), mkdir, create,
 *           unlink, rename. Buffer cache used for all sector I/O.
 *
 * Assumptions: bytes_per_sector == 512 (standard for all real FAT32 volumes).
 */
#include "fat32.h"
#include "fs/vfs.h"
#include "fs/bcache.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

#include <stdint.h>
#include <stdbool.h>

/* ── Constants ───────────────────────────────────────────────────────────── */
#define FAT32_SECTOR_SIZE   512
#define FAT32_DIR_ENTRY_SZ  32
#define FAT32_EOC           0x0FFFFFF8U
#define FAT32_FREE          0x00000000U
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0F     /* Long File Name entry */
#define FAT32_DELETED        0xE5
#define FAT32_LAST_LFN       0x40

/* ── On-disk BPB (partial, 512-byte sector 0) ─────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;  /* FAT32: must be 0 */
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t fat_size_16;       /* FAT32: must be 0 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_num;
    uint8_t  reserved1;
    uint8_t  boot_sig;          /* 0x28 or 0x29 */
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];        /* "FAT32   " */
} fat32_bpb_t;

/* ── On-disk directory entry ─────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t acc_date;
    uint16_t cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t cluster_lo;
    uint32_t file_size;
} fat32_direntry_t;

/* ── LFN directory entry ─────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  seq;          /* bit6=last, bits[4:0]=index (1-based)          */
    uint16_t name1[5];
    uint8_t  attr;         /* must be 0x0F                                  */
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t cluster;      /* must be 0                                     */
    uint16_t name3[2];
} fat32_lfn_t;

/* ── Filesystem instance private data ───────────────────────────────────── */
typedef struct {
    blkdev_t *dev;
    uint32_t  fat_lba;            /* first sector of FAT                    */
    uint32_t  data_lba;           /* first sector of data region            */
    uint32_t  root_cluster;
    uint32_t  sectors_per_cluster;
    uint32_t  bytes_per_cluster;
    uint32_t  total_clusters;
    uint32_t  fat_size;           /* sectors per FAT                        */
    uint32_t  uid;
    uint32_t  gid;
    uint16_t  fmask;
    uint16_t  dmask;
} fat32_sb_t;

typedef struct {
    uint32_t uid;
    uint32_t gid;
    uint16_t fmask;
    uint16_t dmask;
} fat32_mount_opts_t;

static fat32_mount_opts_t g_fat32_mount_opts = {
    .uid = 0,
    .gid = 0,
    .fmask = 022,
    .dmask = 022,
};

static int fat32_parse_u32(const char *s, uint32_t *out, int octal_only) {
    if (!s || !s[0] || !out) return -EINVAL;
    uint64_t v = 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return -EINVAL;
        if (octal_only && c > '7') return -EINVAL;
        int base = octal_only ? 8 : 10;
        v = v * (uint64_t)base + (uint64_t)(c - '0');
        if (v > 0xFFFFFFFFULL) return -EINVAL;
    }
    *out = (uint32_t)v;
    return 0;
}

void fat32_set_mount_opts(const char *opts) {
    fat32_mount_opts_t parsed = g_fat32_mount_opts;
    parsed.uid = 0;
    parsed.gid = 0;
    parsed.fmask = 022;
    parsed.dmask = 022;

    if (opts && opts[0]) {
        char tmp[256];
        strncpy(tmp, opts, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        char *cur = tmp;
        while (*cur) {
            char *tok = cur;
            while (*cur && *cur != ',') cur++;
            if (*cur == ',') *cur++ = '\0';
            if (!tok[0]) continue;

            char *eq = tok;
            while (*eq && *eq != '=') eq++;
            if (*eq != '=') continue;
            *eq++ = '\0';

            uint32_t val = 0;
            if (strcmp(tok, "uid") == 0) {
                if (fat32_parse_u32(eq, &val, 0) == 0) parsed.uid = val;
            } else if (strcmp(tok, "gid") == 0) {
                if (fat32_parse_u32(eq, &val, 0) == 0) parsed.gid = val;
            } else if (strcmp(tok, "fmask") == 0) {
                if (fat32_parse_u32(eq, &val, 1) == 0) parsed.fmask = (uint16_t)(val & 0777);
            } else if (strcmp(tok, "dmask") == 0) {
                if (fat32_parse_u32(eq, &val, 1) == 0) parsed.dmask = (uint16_t)(val & 0777);
            }
        }
    }

    g_fat32_mount_opts = parsed;
}

/* ── Per-file private data (stored in vnode.fs_data) ─────────────────────── */
typedef struct {
    uint32_t first_cluster;
    uint32_t parent_cluster;  /* cluster of parent directory                */
    uint32_t dir_offset;      /* byte offset of this entry in parent dir    */
} fat32_node_t;

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static uint64_t cluster_to_lba(fat32_sb_t *sb, uint32_t cluster) {
    return (uint64_t)sb->data_lba + (uint64_t)(cluster - 2) * sb->sectors_per_cluster;
}

/* Read the FAT entry for `cluster`. Returns next cluster or 0 on error. */
static uint32_t fat_read(fat32_sb_t *sb, uint32_t cluster) {
    uint32_t fat_sector_off = (cluster * 4) / FAT32_SECTOR_SIZE;
    uint32_t fat_byte_off   = (cluster * 4) % FAT32_SECTOR_SIZE;

    bcache_buf_t *b = bcache_get(sb->dev, sb->fat_lba + fat_sector_off);
    if (!b) return 0;

    uint32_t val;
    memcpy(&val, b->data + fat_byte_off, 4);
    bcache_release(b);
    return val & 0x0FFFFFFFU;
}

/* Write FAT entry for `cluster` → `value`. Returns 0 on success. */
static int fat_write(fat32_sb_t *sb, uint32_t cluster, uint32_t value) {
    uint32_t fat_sector_off = (cluster * 4) / FAT32_SECTOR_SIZE;
    uint32_t fat_byte_off   = (cluster * 4) % FAT32_SECTOR_SIZE;

    bcache_buf_t *b = bcache_get(sb->dev, sb->fat_lba + fat_sector_off);
    if (!b) return -EIO;

    uint32_t existing;
    memcpy(&existing, b->data + fat_byte_off, 4);
    existing = (existing & 0xF0000000U) | (value & 0x0FFFFFFFU);
    memcpy(b->data + fat_byte_off, &existing, 4);
    bcache_mark_dirty(b);
    bcache_release(b);
    return 0;
}

/* Allocate a free cluster. Returns cluster number or 0 on failure (ENOSPC). */
static uint32_t fat_alloc_cluster(fat32_sb_t *sb) {
    for (uint32_t c = 2; c < sb->total_clusters + 2; c++) {
        if (fat_read(sb, c) == FAT32_FREE) {
            fat_write(sb, c, 0x0FFFFFFFU); /* EOC */
            return c;
        }
    }
    return 0; /* disk full */
}

/* Free the entire cluster chain starting at `start`. */
static void fat_free_chain(fat32_sb_t *sb, uint32_t start) {
    uint32_t c = start;
    while (c >= 2 && c < 0x0FFFFFF8U) {
        uint32_t next = fat_read(sb, c);
        fat_write(sb, c, FAT32_FREE);
        c = next;
    }
}

/* Follow FAT chain from `start`, skipping `skip` clusters.
 * Returns destination cluster or 0xFFFFFFFF at EOC/error. */
static uint32_t fat_follow(fat32_sb_t *sb, uint32_t start, uint32_t skip) {
    uint32_t c = start;
    for (uint32_t i = 0; i < skip; i++) {
        if (c < 2 || c >= 0x0FFFFFF8U) return 0xFFFFFFFF;
        c = fat_read(sb, c);
    }
    return c;
}

/* Append a new cluster at the end of chain starting at `first_cluster`.
 * If chain is empty (first_cluster==0), just returns new cluster. */
static uint32_t fat_chain_extend(fat32_sb_t *sb, uint32_t first_cluster) {
    uint32_t new_c = fat_alloc_cluster(sb);
    if (!new_c) return 0;

    if (first_cluster < 2) return new_c;

    /* Find end of chain */
    uint32_t cur = first_cluster;
    while (true) {
        uint32_t next = fat_read(sb, cur);
        if (next >= 0x0FFFFFF8U) break;
        if (next < 2) break;
        cur = next;
    }
    fat_write(sb, cur, new_c);
    return new_c;
}

/* Zero-fill a cluster (for new dirs/files). */
static void cluster_zero(fat32_sb_t *sb, uint32_t cluster) {
    uint64_t lba = cluster_to_lba(sb, cluster);
    for (uint32_t s = 0; s < sb->sectors_per_cluster; s++) {
        bcache_buf_t *b = bcache_get(sb->dev, lba + s);
        if (b) {
            memset(b->data, 0, FAT32_SECTOR_SIZE);
            bcache_mark_dirty(b);
            bcache_release(b);
        }
    }
}

/* ── LFN name assembly ───────────────────────────────────────────────────── */
/* Collect up to 20 LFN entries (20 * 13 = 260 chars) into a UTF-16 buffer,
 * then convert to ASCII. Returns length of assembled name. */
static int lfn_to_ascii(fat32_lfn_t *lfns, int count, char *out, int outsz) {
    /* LFN entries are stored in reverse order (last entry first in dir).
     * seq number (1-based) tells the order. Max 20 entries. */
    uint16_t buf[20 * 13 + 1];
    int len = 0;

    for (int i = count - 1; i >= 0; i--) {
        fat32_lfn_t *e = &lfns[i];
        uint16_t chars[13];
        memcpy(chars + 0, e->name1, 10);
        memcpy(chars + 5, e->name2, 12);
        memcpy(chars + 11, e->name3, 4);
        for (int j = 0; j < 13; j++) {
            if (chars[j] == 0x0000) goto done;
            if (chars[j] == 0xFFFF) continue;
            if (len < 20 * 13) buf[len++] = chars[j];
        }
    }
done:
    buf[len] = 0;
    int olen = 0;
    for (int i = 0; i < len && olen < outsz - 1; i++)
        out[olen++] = (buf[i] < 0x80) ? (char)buf[i] : '?';
    out[olen] = '\0';
    return olen;
}

/* Decode an 8.3 short name (name[8]+ext[3]) into a human-readable string. */
static void shortname_to_str(const uint8_t *name, const uint8_t *ext, char *out) {
    int n = 0;
    /* Name: trim trailing spaces */
    int nlen = 8;
    while (nlen > 0 && name[nlen-1] == ' ') nlen--;
    for (int i = 0; i < nlen; i++) out[n++] = (char)name[i];
    /* Extension */
    int elen = 3;
    while (elen > 0 && ext[elen-1] == ' ') elen--;
    if (elen > 0) {
        out[n++] = '.';
        for (int i = 0; i < elen; i++) out[n++] = (char)ext[i];
    }
    out[n] = '\0';
}

/* ── Directory iteration ─────────────────────────────────────────────────── */
/*
 * Reads directory entries from cluster chain.
 * cookie = byte offset within the directory.
 * Assembles LFN names, skips deleted/volume-id entries.
 * Fills out->ino with first_cluster of the entry, out->name with name.
 * Returns 1 found, 0 EOF, -1 error.
 */
typedef struct {
    char     name[VFS_NAME_MAX + 1]; /* assembled name */
    uint32_t first_cluster;
    uint32_t file_size;
    uint8_t  attr;
    uint32_t dir_cluster;  /* cluster containing this entry */
    uint32_t dir_offset;   /* byte offset within that cluster */
} fat32_dirent_result_t;

static int fat32_dir_next(fat32_sb_t *sb, uint32_t dir_cluster,
                           uint64_t *cookie, fat32_dirent_result_t *out) {
    fat32_lfn_t lfns[20];
    int lfn_count = 0;

    uint64_t boff = *cookie;

    while (true) {
        uint32_t bytes_per_cluster = sb->sectors_per_cluster * FAT32_SECTOR_SIZE;

        /* Determine which cluster and offset within it */
        uint32_t cluster_idx = (uint32_t)(boff / bytes_per_cluster);
        uint32_t cluster_off = (uint32_t)(boff % bytes_per_cluster);

        uint32_t cur_cluster = fat_follow(sb, dir_cluster, cluster_idx);
        if (cur_cluster == 0 || cur_cluster >= 0x0FFFFFF8U) return 0; /* EOF */

        uint32_t sector_idx = cluster_off / FAT32_SECTOR_SIZE;
        uint32_t sector_off = cluster_off % FAT32_SECTOR_SIZE;

        uint64_t lba = cluster_to_lba(sb, cur_cluster) + sector_idx;
        bcache_buf_t *b = bcache_get(sb->dev, lba);
        if (!b) return -1;

        fat32_direntry_t *entry = (fat32_direntry_t *)(b->data + sector_off);

        if (entry->name[0] == 0x00) {
            /* End of directory */
            bcache_release(b);
            return 0;
        }

        uint32_t save_cluster = cur_cluster;
        uint32_t save_offset  = cluster_off;

        if ((uint8_t)entry->name[0] == FAT32_DELETED) {
            bcache_release(b);
            boff += FAT32_DIR_ENTRY_SZ;
            lfn_count = 0;
            continue;
        }

        if (entry->attr == FAT32_ATTR_LFN) {
            /* LFN entry — copy into lfns buffer */
            if (lfn_count < 20) {
                fat32_lfn_t *lfn = (fat32_lfn_t *)entry;
                /* Sequence number bits[4:0] give 1-based index */
                int seq = (lfn->seq & ~FAT32_LAST_LFN) - 1;
                if (seq >= 0 && seq < 20)
                    memcpy(&lfns[seq], lfn, sizeof(fat32_lfn_t));
                lfn_count++;
            }
            bcache_release(b);
            boff += FAT32_DIR_ENTRY_SZ;
            continue;
        }

        /* Skip volume-ID entries */
        if (entry->attr & FAT32_ATTR_VOLUME_ID) {
            bcache_release(b);
            boff += FAT32_DIR_ENTRY_SZ;
            lfn_count = 0;
            continue;
        }

        /* Assemble entry */
        if (lfn_count > 0) {
            lfn_to_ascii(lfns, lfn_count, out->name, VFS_NAME_MAX + 1);
            lfn_count = 0;
        } else {
            shortname_to_str(entry->name, entry->ext, out->name);
        }

        out->first_cluster = ((uint32_t)entry->cluster_hi << 16) |
                              (uint32_t)entry->cluster_lo;
        out->file_size     = entry->file_size;
        out->attr          = entry->attr;
        out->dir_cluster   = save_cluster;
        out->dir_offset    = save_offset;

        bcache_release(b);
        boff += FAT32_DIR_ENTRY_SZ;
        *cookie = boff;
        return 1;
    }
}

/* ── Lookup a name in a directory cluster chain ──────────────────────────── */
static int fat32_dir_lookup(fat32_sb_t *sb, uint32_t dir_cluster,
                             const char *name, fat32_dirent_result_t *out) {
    uint64_t cookie = 0;
    int r;
    while ((r = fat32_dir_next(sb, dir_cluster, &cookie, out)) == 1) {
        if (strcmp(out->name, name) == 0) return 1; /* found */
    }
    return 0; /* not found */
}

/* ── Build vnode from a directory result ─────────────────────────────────── */
static vnode_t *make_vnode(fat32_sb_t *sb, fs_inst_t *fsi,
                            const fat32_dirent_result_t *e) {
    vnode_t     *v    = vfs_alloc_vnode();
    fat32_node_t *nd  = kzalloc(sizeof(fat32_node_t));
    if (!v || !nd) { kfree(v); kfree(nd); return NULL; }

    nd->first_cluster  = e->first_cluster;
    nd->parent_cluster = e->dir_cluster;
    nd->dir_offset     = e->dir_offset;

    v->ino     = e->first_cluster ? e->first_cluster : 0xFFFF;
    v->size    = e->file_size;
    if (e->attr & FAT32_ATTR_DIRECTORY)
        v->mode = VFS_S_IFDIR | (0777 & ~(uint32_t)(sb->dmask & 0777));
    else
        v->mode = VFS_S_IFREG | (0666 & ~(uint32_t)(sb->fmask & 0777));
    v->uid     = sb->uid;
    v->gid     = sb->gid;
    v->ops     = &g_fat32_ops;
    v->fsi     = fsi;
    v->fs_data = nd;
    v->refcount = 1;
    return v;
}

/* ── vnode → fat32_sb_t ──────────────────────────────────────────────────── */
static inline fat32_sb_t *get_sb(vnode_t *v) {
    return (fat32_sb_t *)v->fsi->priv;
}

/* ── fs_ops implementations ──────────────────────────────────────────────── */
static vnode_t *fat32_lookup(vnode_t *dir, const char *name) {
    fat32_sb_t   *sb  = get_sb(dir);
    fat32_node_t *dnd = (fat32_node_t *)dir->fs_data;
    uint32_t dir_cluster = dnd->first_cluster;
    if (dir_cluster < 2) dir_cluster = sb->root_cluster;

    fat32_dirent_result_t res;
    if (!fat32_dir_lookup(sb, dir_cluster, name, &res)) return NULL;

    return make_vnode(sb, dir->fsi, &res);
}

static int fat32_open(vnode_t *v, int flags) { (void)v; (void)flags; return 0; }
static int fat32_close(vnode_t *v) { (void)v; return 0; }

static ssize_t fat32_read(vnode_t *v, void *buf, size_t len, uint64_t off) {
    fat32_sb_t   *sb  = get_sb(v);
    fat32_node_t *nd  = (fat32_node_t *)v->fs_data;

    if (off >= v->size) return 0;
    if (off + len > v->size) len = (size_t)(v->size - off);

    uint32_t bpc = sb->bytes_per_cluster;
    uint8_t *dst = (uint8_t *)buf;
    size_t   done = 0;

    while (done < len) {
        uint32_t cluster_idx = (uint32_t)((off + done) / bpc);
        uint32_t cluster_off = (uint32_t)((off + done) % bpc);
        uint32_t cur = fat_follow(sb, nd->first_cluster, cluster_idx);
        if (cur < 2 || cur >= 0x0FFFFFF8U) break;

        size_t chunk = bpc - cluster_off;
        if (chunk > len - done) chunk = len - done;

        uint64_t lba = cluster_to_lba(sb, cur) + cluster_off / FAT32_SECTOR_SIZE;
        uint32_t sec_off = cluster_off % FAT32_SECTOR_SIZE;

        while (chunk > 0) {
            bcache_buf_t *b = bcache_get(sb->dev, lba);
            if (!b) return done == 0 ? -EIO : (ssize_t)done;
            size_t copy = FAT32_SECTOR_SIZE - sec_off;
            if (copy > chunk) copy = chunk;
            memcpy(dst + done, b->data + sec_off, copy);
            bcache_release(b);
            done += copy; chunk -= copy;
            lba++; sec_off = 0;
        }
    }
    return (ssize_t)done;
}

static ssize_t fat32_write(vnode_t *v, const void *buf, size_t len, uint64_t off) {
    if (len == 0) return 0;
    fat32_sb_t   *sb = get_sb(v);
    fat32_node_t *nd = (fat32_node_t *)v->fs_data;

    uint32_t bpc = sb->bytes_per_cluster;
    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;

    while (done < len) {
        uint64_t cur_off = off + done;
        uint32_t cluster_idx = (uint32_t)(cur_off / bpc);
        uint32_t cluster_off = (uint32_t)(cur_off % bpc);

        /* Follow / extend chain */
        uint32_t cur = nd->first_cluster;
        if (cur < 2) {
            /* File has no cluster yet — allocate first */
            cur = fat_alloc_cluster(sb);
            if (!cur) return done == 0 ? -ENOSPC : (ssize_t)done;
            nd->first_cluster = cur;
            cluster_zero(sb, cur);
        }
        for (uint32_t i = 0; i < cluster_idx; i++) {
            uint32_t next = fat_read(sb, cur);
            if (next >= 0x0FFFFFF8U) {
                next = fat_chain_extend(sb, cur);
                if (!next) return done == 0 ? -ENOSPC : (ssize_t)done;
                cluster_zero(sb, next);
            }
            cur = next;
        }

        size_t chunk = bpc - cluster_off;
        if (chunk > len - done) chunk = len - done;

        uint64_t lba = cluster_to_lba(sb, cur) + cluster_off / FAT32_SECTOR_SIZE;
        uint32_t sec_off = cluster_off % FAT32_SECTOR_SIZE;

        while (chunk > 0) {
            bcache_buf_t *b = bcache_get(sb->dev, lba);
            if (!b) return done == 0 ? -EIO : (ssize_t)done;
            size_t copy = FAT32_SECTOR_SIZE - sec_off;
            if (copy > chunk) copy = chunk;
            memcpy(b->data + sec_off, src + done, copy);
            bcache_mark_dirty(b);
            bcache_release(b);
            done += copy; chunk -= copy;
            lba++; sec_off = 0;
        }
    }

    /* Update file size if grown */
    if (off + done > v->size) {
        v->size = off + done;
        /* Update directory entry size */
        fat32_node_t *nd2 = (fat32_node_t *)v->fs_data;
        /* Cluster containing the dir entry */
        uint32_t dir_sec_idx = nd2->dir_offset / FAT32_SECTOR_SIZE;
        uint32_t dir_sec_off = nd2->dir_offset % FAT32_SECTOR_SIZE;
        uint64_t dir_lba = cluster_to_lba(sb, nd2->parent_cluster) + dir_sec_idx;
        bcache_buf_t *b = bcache_get(sb->dev, dir_lba);
        if (b) {
            fat32_direntry_t *de = (fat32_direntry_t *)(b->data + dir_sec_off);
            de->file_size   = (uint32_t)v->size;
            de->cluster_hi  = (uint16_t)(nd->first_cluster >> 16);
            de->cluster_lo  = (uint16_t)(nd->first_cluster & 0xFFFF);
            bcache_mark_dirty(b);
            bcache_release(b);
        }
    }
    return (ssize_t)done;
}

static int fat32_readdir(vnode_t *dir, uint64_t *cookie, vfs_dirent_t *out) {
    fat32_sb_t   *sb  = get_sb(dir);
    fat32_node_t *dnd = (fat32_node_t *)dir->fs_data;
    uint32_t dir_cluster = dnd->first_cluster;
    if (dir_cluster < 2) dir_cluster = sb->root_cluster;

    fat32_dirent_result_t res;
    int r = fat32_dir_next(sb, dir_cluster, cookie, &res);
    if (r != 1) return r;

    out->ino  = res.first_cluster;
    out->type = (res.attr & FAT32_ATTR_DIRECTORY) ? VFS_DT_DIR : VFS_DT_REG;
    strncpy(out->name, res.name, VFS_NAME_MAX);
    return 1;
}

/* Write a short-name directory entry into parent dir cluster chain.
 * parent_cluster: chain start of parent directory.
 * Returns byte offset within the parent dir stream on success, or -1. */
static int32_t fat32_alloc_dir_entry(fat32_sb_t *sb, uint32_t parent_cluster,
                                      fat32_direntry_t *entry) {
    uint32_t bpc = sb->bytes_per_cluster;

    /* Scan for a free (0xE5 or 0x00) or empty slot */
    uint32_t bytes_per_cluster = bpc;
    for (uint32_t ci = 0; ; ci++) {
        uint32_t cur = fat_follow(sb, parent_cluster, ci);
        if (cur < 2 || cur >= 0x0FFFFFF8U) {
            /* Extend parent dir cluster chain */
            cur = fat_chain_extend(sb, parent_cluster);
            if (!cur) return -1;
            cluster_zero(sb, cur);
        }

        uint64_t base_lba = cluster_to_lba(sb, cur);
        for (uint32_t si = 0; si < sb->sectors_per_cluster; si++) {
            uint64_t lba = base_lba + si;
            bcache_buf_t *b = bcache_get(sb->dev, lba);
            if (!b) return -1;

            for (int oi = 0; oi + FAT32_DIR_ENTRY_SZ <= FAT32_SECTOR_SIZE;
                 oi += FAT32_DIR_ENTRY_SZ) {
                uint8_t first = b->data[oi];
                if (first == 0x00 || first == FAT32_DELETED) {
                    /* Use this slot */
                    memcpy(b->data + oi, entry, FAT32_DIR_ENTRY_SZ);
                    bcache_mark_dirty(b);
                    bcache_release(b);
                    /* Return the absolute offset within the parent dir stream */
                    uint32_t abs_off = ci * bytes_per_cluster
                                     + si * FAT32_SECTOR_SIZE + oi;
                    return (int32_t)abs_off;
                }
            }
            bcache_release(b);
        }
    }
}

/* Build a FAT 8.3 short name from a long name. */
static void build_short_name(const char *name, uint8_t *sname, uint8_t *sext) {
    memset(sname, ' ', 8);
    memset(sext,  ' ', 3);
    int ni = 0, ei = 0;
    bool in_ext = false;
    for (int i = 0; name[i] && ni < 8; i++) {
        char c = name[i];
        if (c == '.') { in_ext = true; continue; }
        if (c >= 'a' && c <= 'z') c -= 32;
        if (in_ext) { if (ei < 3) sext[ei++] = (uint8_t)c; }
        else        {              sname[ni++] = (uint8_t)c; }
    }
}

static vnode_t *fat32_create(vnode_t *parent, const char *name, uint32_t mode) {
    fat32_sb_t   *sb  = get_sb(parent);
    fat32_node_t *dnd = (fat32_node_t *)parent->fs_data;
    uint32_t dir_cluster = dnd->first_cluster;
    if (dir_cluster < 2) dir_cluster = sb->root_cluster;

    fat32_direntry_t de = {0};
    build_short_name(name, de.name, de.ext);
    de.attr      = FAT32_ATTR_ARCHIVE;
    de.file_size = 0;
    /* No cluster allocated yet — first write will allocate */
    de.cluster_hi = 0;
    de.cluster_lo = 0;

    int32_t offset = fat32_alloc_dir_entry(sb, dir_cluster, &de);
    if (offset < 0) return NULL;

    fat32_dirent_result_t res = {
        .first_cluster = 0,
        .file_size     = 0,
        .attr          = FAT32_ATTR_ARCHIVE,
        .dir_cluster   = dir_cluster,
        .dir_offset    = (uint32_t)(offset % (sb->sectors_per_cluster * FAT32_SECTOR_SIZE)),
    };
    strncpy(res.name, name, VFS_NAME_MAX);
    /* Store the actual LBA cluster */
    res.dir_cluster = fat_follow(sb, dir_cluster,
                                  (uint32_t)offset / (sb->sectors_per_cluster * FAT32_SECTOR_SIZE));

    return make_vnode(sb, parent->fsi, &res);
}

static vnode_t *fat32_mkdir(vnode_t *parent, const char *name, uint32_t mode) {
    fat32_sb_t   *sb  = get_sb(parent);
    fat32_node_t *dnd = (fat32_node_t *)parent->fs_data;
    uint32_t dir_cluster = dnd->first_cluster;
    if (dir_cluster < 2) dir_cluster = sb->root_cluster;

    /* Allocate a cluster for the new directory */
    uint32_t new_cluster = fat_alloc_cluster(sb);
    if (!new_cluster) return NULL;
    cluster_zero(sb, new_cluster);

    /* Create "." and ".." entries */
    uint64_t lba = cluster_to_lba(sb, new_cluster);
    bcache_buf_t *b = bcache_get(sb->dev, lba);
    if (!b) { fat_write(sb, new_cluster, FAT32_FREE); return NULL; }

    fat32_direntry_t *dot = (fat32_direntry_t *)b->data;
    memset(dot, ' ', 8); dot->name[0]='.';
    memset(dot->ext, ' ', 3);
    dot->attr       = FAT32_ATTR_DIRECTORY;
    dot->cluster_hi = (uint16_t)(new_cluster >> 16);
    dot->cluster_lo = (uint16_t)(new_cluster & 0xFFFF);

    fat32_direntry_t *dotdot = dot + 1;
    memset(dotdot->name, ' ', 8); dotdot->name[0]='.'; dotdot->name[1]='.';
    memset(dotdot->ext,  ' ', 3);
    dotdot->attr       = FAT32_ATTR_DIRECTORY;
    dotdot->cluster_hi = (uint16_t)(dir_cluster >> 16);
    dotdot->cluster_lo = (uint16_t)(dir_cluster & 0xFFFF);

    bcache_mark_dirty(b);
    bcache_release(b);

    /* Create directory entry in parent */
    fat32_direntry_t de = {0};
    build_short_name(name, de.name, de.ext);
    de.attr       = FAT32_ATTR_DIRECTORY;
    de.cluster_hi = (uint16_t)(new_cluster >> 16);
    de.cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
    de.file_size  = 0;

    int32_t offset = fat32_alloc_dir_entry(sb, dir_cluster, &de);
    if (offset < 0) { fat_free_chain(sb, new_cluster); return NULL; }

    fat32_dirent_result_t res = {
        .first_cluster = new_cluster,
        .file_size     = 0,
        .attr          = FAT32_ATTR_DIRECTORY,
        .dir_cluster   = fat_follow(sb, dir_cluster,
                         (uint32_t)offset / (sb->sectors_per_cluster*FAT32_SECTOR_SIZE)),
        .dir_offset    = (uint32_t)(offset % (sb->sectors_per_cluster*FAT32_SECTOR_SIZE)),
    };
    strncpy(res.name, name, VFS_NAME_MAX);
    return make_vnode(sb, parent->fsi, &res);
}

static int fat32_unlink(vnode_t *parent, const char *name) {
    fat32_sb_t   *sb  = get_sb(parent);
    fat32_node_t *dnd = (fat32_node_t *)parent->fs_data;
    uint32_t dir_cluster = dnd->first_cluster;
    if (dir_cluster < 2) dir_cluster = sb->root_cluster;

    fat32_dirent_result_t res;
    if (!fat32_dir_lookup(sb, dir_cluster, name, &res)) return -ENOENT;

    /* Mark dir entry as deleted */
    uint64_t lba = cluster_to_lba(sb, res.dir_cluster)
                   + res.dir_offset / FAT32_SECTOR_SIZE;
    uint32_t sec_off = res.dir_offset % FAT32_SECTOR_SIZE;

    bcache_buf_t *b = bcache_get(sb->dev, lba);
    if (!b) return -EIO;
    b->data[sec_off] = FAT32_DELETED;
    bcache_mark_dirty(b);
    bcache_release(b);

    /* Free cluster chain */
    if (res.first_cluster >= 2 && res.first_cluster < 0x0FFFFFF8U)
        fat_free_chain(sb, res.first_cluster);

    return 0;
}

static int fat32_rmdir(vnode_t *parent, const char *name) {
    /* For simplicity, unlink as well (caller should check emptiness) */
    return fat32_unlink(parent, name);
}

static int fat32_stat(vnode_t *v, vfs_stat_t *st) {
    memset(st, 0, sizeof(*st));
    st->ino   = v->ino;
    st->mode  = v->mode;
    st->size  = v->size;
    st->nlink = 1;
    return 0;
}

static int fat32_rename(vnode_t *old_dir, const char *old_name,
                         vnode_t *new_dir, const char *new_name) {
    /* Simple: create new entry then unlink old */
    fat32_sb_t   *sb     = get_sb(old_dir);
    fat32_node_t *old_nd = (fat32_node_t *)old_dir->fs_data;
    fat32_node_t *new_nd = (fat32_node_t *)new_dir->fs_data;
    uint32_t old_cluster = old_nd->first_cluster;
    if (old_cluster < 2) old_cluster = sb->root_cluster;
    uint32_t new_cluster = new_nd->first_cluster;
    if (new_cluster < 2) new_cluster = sb->root_cluster;

    fat32_dirent_result_t res;
    if (!fat32_dir_lookup(sb, old_cluster, old_name, &res)) return -ENOENT;

    /* Create entry in new parent */
    fat32_direntry_t de = {0};
    build_short_name(new_name, de.name, de.ext);
    de.attr       = res.attr;
    de.cluster_hi = (uint16_t)(res.first_cluster >> 16);
    de.cluster_lo = (uint16_t)(res.first_cluster & 0xFFFF);
    de.file_size  = res.file_size;

    if (fat32_alloc_dir_entry(sb, new_cluster, &de) < 0) return -ENOSPC;

    /* Delete old entry */
    uint64_t lba = cluster_to_lba(sb, res.dir_cluster)
                   + res.dir_offset / FAT32_SECTOR_SIZE;
    bcache_buf_t *b = bcache_get(sb->dev, lba);
    if (b) {
        b->data[res.dir_offset % FAT32_SECTOR_SIZE] = FAT32_DELETED;
        bcache_mark_dirty(b);
        bcache_release(b);
    }
    return 0;
}

static int fat32_truncate(vnode_t *v, uint64_t size) {
    fat32_sb_t   *sb = get_sb(v);
    fat32_node_t *nd = (fat32_node_t *)v->fs_data;

    if (size == 0) {
        if (nd->first_cluster >= 2) fat_free_chain(sb, nd->first_cluster);
        nd->first_cluster = 0;
        v->size = 0;
    } else {
        /* Truncate to `size` — keep ceil(size/bpc) clusters */
        uint32_t bpc = sb->bytes_per_cluster;
        uint32_t keep = ((uint32_t)size + bpc - 1) / bpc;
        uint32_t cur = nd->first_cluster;
        for (uint32_t i = 1; i < keep && cur < 0x0FFFFFF8U; i++) cur = fat_read(sb, cur);
        if (cur >= 2 && cur < 0x0FFFFFF8U) {
            uint32_t next = fat_read(sb, cur);
            fat_write(sb, cur, 0x0FFFFFF8U); /* mark EOC */
            if (next < 0x0FFFFFF8U) fat_free_chain(sb, next);
        }
        v->size = size;
    }

    /* Update directory entry */
    uint64_t lba = cluster_to_lba(sb, nd->parent_cluster)
                   + nd->dir_offset / FAT32_SECTOR_SIZE;
    bcache_buf_t *b = bcache_get(sb->dev, lba);
    if (b) {
        fat32_direntry_t *de = (fat32_direntry_t *)(b->data + nd->dir_offset % FAT32_SECTOR_SIZE);
        de->file_size   = (uint32_t)v->size;
        de->cluster_hi  = (uint16_t)(nd->first_cluster >> 16);
        de->cluster_lo  = (uint16_t)(nd->first_cluster & 0xFFFF);
        bcache_mark_dirty(b);
        bcache_release(b);
    }
    return 0;
}

static int fat32_sync_v(vnode_t *v) { (void)v; return 0; }

static void fat32_evict(vnode_t *v) {
    if (v->fs_data) { kfree(v->fs_data); v->fs_data = NULL; }
}

/* ── Mount ───────────────────────────────────────────────────────────────── */
static vnode_t *fat32_mount(fs_inst_t *fsi, blkdev_t *dev) {
    /* Read sector 0 (BPB) */
    bcache_buf_t *b = bcache_get(dev, 0);
    if (!b) return NULL;

    fat32_bpb_t bpb;
    memcpy(&bpb, b->data, sizeof(fat32_bpb_t));
    bcache_release(b);

    /* Validate FAT32 signature */
    if (bpb.boot_sig != 0x28 && bpb.boot_sig != 0x29) {
        KLOG_WARN("fat32: invalid boot signature on '%s'\n", dev->name);
        return NULL;
    }
    if (bpb.bytes_per_sector != FAT32_SECTOR_SIZE) {
        KLOG_WARN("fat32: sector size %u not supported\n", bpb.bytes_per_sector);
        return NULL;
    }
    if (bpb.fat_size_32 == 0 || bpb.root_cluster == 0) {
        KLOG_WARN("fat32: not a FAT32 volume on '%s'\n", dev->name);
        return NULL;
    }

    fat32_sb_t *sb = kzalloc(sizeof(fat32_sb_t));
    if (!sb) return NULL;

    sb->dev = dev;
    sb->sectors_per_cluster = bpb.sectors_per_cluster;
    sb->bytes_per_cluster   = bpb.sectors_per_cluster * FAT32_SECTOR_SIZE;
    sb->fat_lba             = bpb.reserved_sectors;
    sb->data_lba            = bpb.reserved_sectors
                            + (uint32_t)bpb.num_fats * bpb.fat_size_32;
    sb->root_cluster        = bpb.root_cluster;
    sb->fat_size            = bpb.fat_size_32;
    sb->uid                 = g_fat32_mount_opts.uid;
    sb->gid                 = g_fat32_mount_opts.gid;
    sb->fmask               = g_fat32_mount_opts.fmask;
    sb->dmask               = g_fat32_mount_opts.dmask;

    uint32_t total_sectors  = bpb.total_sectors_32 ? bpb.total_sectors_32
                                                    : bpb.total_sectors_16;
    sb->total_clusters      = (total_sectors - sb->data_lba) / sb->sectors_per_cluster;

    fsi->priv = sb;

    KLOG_INFO("fat32: '%s' root_cluster=%u data_lba=%u spc=%u\n",
              dev->name, sb->root_cluster, sb->data_lba, sb->sectors_per_cluster);

    /* Build root vnode */
    fat32_node_t *rnd = kzalloc(sizeof(fat32_node_t));
    if (!rnd) { kfree(sb); return NULL; }
    rnd->first_cluster  = sb->root_cluster;
    rnd->parent_cluster = sb->root_cluster;
    rnd->dir_offset     = 0;

    vnode_t *root = vfs_alloc_vnode();
    if (!root) { kfree(rnd); kfree(sb); return NULL; }
    root->ino      = sb->root_cluster;
    root->mode     = VFS_S_IFDIR | (0777 & ~(uint32_t)(sb->dmask & 0777));
    root->uid      = sb->uid;
    root->gid      = sb->gid;
    root->ops      = &g_fat32_ops;
    root->fsi      = fsi;
    root->fs_data  = rnd;
    root->refcount = 1;
    return root;
}

static void fat32_unmount(fs_inst_t *fsi) {
    bcache_flush_dev(((fat32_sb_t *)fsi->priv)->dev);
    kfree(fsi->priv);
    fsi->priv = NULL;
}

/* ── fs_ops vtable ───────────────────────────────────────────────────────── */
fs_ops_t g_fat32_ops = {
    .name     = "fat32",
    .lookup   = fat32_lookup,
    .open     = fat32_open,
    .close    = fat32_close,
    .read     = fat32_read,
    .write    = fat32_write,
    .readdir  = fat32_readdir,
    .create   = fat32_create,
    .mkdir    = fat32_mkdir,
    .unlink   = fat32_unlink,
    .rmdir    = fat32_rmdir,
    .rename   = fat32_rename,
    .stat     = fat32_stat,
    .symlink  = NULL,
    .readlink = NULL,
    .truncate = fat32_truncate,
    .sync     = fat32_sync_v,
    .evict    = fat32_evict,
    .mount    = fat32_mount,
    .unmount  = fat32_unmount,
};

void fat32_register(void) {
    vfs_register_fs(&g_fat32_ops);
}
