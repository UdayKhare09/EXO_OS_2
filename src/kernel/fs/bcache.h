/* fs/bcache.h — Kernel block buffer cache
 *
 * The buffer cache sits between filesystems and storage drivers.
 * It caches 512-byte sectors keyed by (dev_id, lba), with LRU eviction
 * and write-back dirty tracking.
 *
 * Usage:
 *   bcache_buf_t *b = bcache_get(dev, lba);   // pin sector into cache
 *   // read/modify b->data[0..511]
 *   bcache_mark_dirty(b);                      // if modified
 *   bcache_release(b);                         // unpin (may evict)
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "drivers/storage/blkdev.h"

/* ── Buffer cache constants ──────────────────────────────────────────────── */
#define BCACHE_SECTOR_SIZE  512
#define BCACHE_NUM_BUFS     512     /* total cached sectors            */
#define BCACHE_HASH_BUCKETS 256     /* power-of-2 for fast modulo      */

/* ── Cache buffer ────────────────────────────────────────────────────────── */
typedef struct bcache_buf {
    uint64_t dev_id;    /* owning device                                    */
    uint64_t lba;       /* logical block address                            */
    uint8_t  data[BCACHE_SECTOR_SIZE];

    bool     dirty;     /* needs write-back                                 */
    bool     valid;     /* data is populated                                */
    uint32_t refcount;  /* number of active pinners (0 = evictable)         */

    /* LRU doubly-linked list (in g_lru) */
    struct bcache_buf *lru_prev;
    struct bcache_buf *lru_next;

    /* Hash-chain singly-linked list */
    struct bcache_buf *hash_next;
} bcache_buf_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Initialise the cache (call once after kmalloc_init). */
void bcache_init(void);

/* Get (and pin) the buffer for sector `lba` on `dev`.
 * If not cached, reads from device. Returns NULL on I/O error / OOM.
 * Caller MUST call bcache_release() when done. */
bcache_buf_t *bcache_get(blkdev_t *dev, uint64_t lba);

/* Mark a pinned buffer as dirty (will be write-back on release/sync). */
void bcache_mark_dirty(bcache_buf_t *buf);

/* Unpin a buffer obtained via bcache_get(). If dirty and refcount reaches 0,
 * schedules write-back (or writes synchronously in this simple impl). */
void bcache_release(bcache_buf_t *buf);

/* Write back all dirty buffers for `dev` (call before unmount). */
void bcache_flush_dev(blkdev_t *dev);

/* Write back ALL dirty buffers across all devices. */
void bcache_sync_all(void);

/* Invalidate all cached buffers for `dev` (e.g. after hot-unplug). */
void bcache_invalidate_dev(blkdev_t *dev);

/* Print cache statistics to the serial log. */
void bcache_print_stats(void);
