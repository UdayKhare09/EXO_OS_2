/* fs/bcache.c — Block buffer cache implementation
 *
 * Fixed pool of BCACHE_NUM_BUFS buffers.
 * Lookup: hash table keyed on (dev_id XOR lba).
 * Eviction: LRU — the buffer at the tail of g_lru is evicted when needed.
 * Write-back: dirty buffers are flushed synchronously on release (simple
 *             write-through model) or via bcache_flush_dev / bcache_sync_all.
 */
#include "bcache.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

/* ── State ───────────────────────────────────────────────────────────────── */
static bcache_buf_t  g_pool[BCACHE_NUM_BUFS];
static bcache_buf_t *g_hash[BCACHE_HASH_BUCKETS]; /* chains through hash_next */

/* LRU list: g_lru_head = most-recently-used, g_lru_tail = evict-candidate  */
static bcache_buf_t *g_lru_head = NULL;
static bcache_buf_t *g_lru_tail = NULL;

static uint64_t g_stat_hits   = 0;
static uint64_t g_stat_misses = 0;
static uint64_t g_stat_evicts = 0;
static uint64_t g_stat_dirty  = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static inline uint32_t hash_key(uint64_t dev_id, uint64_t lba) {
    return (uint32_t)((dev_id * 2654435761ULL) ^ (lba * 2246822519ULL))
           & (BCACHE_HASH_BUCKETS - 1);
}

/* Remove buf from the LRU list */
static void lru_remove(bcache_buf_t *b) {
    if (b->lru_prev) b->lru_prev->lru_next = b->lru_next;
    else             g_lru_head             = b->lru_next;
    if (b->lru_next) b->lru_next->lru_prev = b->lru_prev;
    else             g_lru_tail             = b->lru_prev;
    b->lru_prev = b->lru_next = NULL;
}

/* Push buf to the front (MRU end) of the LRU list */
static void lru_push_front(bcache_buf_t *b) {
    b->lru_prev = NULL;
    b->lru_next = g_lru_head;
    if (g_lru_head) g_lru_head->lru_prev = b;
    g_lru_head = b;
    if (!g_lru_tail) g_lru_tail = b;
}

/* Remove buf from its hash chain */
static void hash_remove(bcache_buf_t *b) {
    if (!b->valid) return;
    uint32_t h = hash_key(b->dev_id, b->lba);
    bcache_buf_t **pp = &g_hash[h];
    while (*pp && *pp != b) pp = &(*pp)->hash_next;
    if (*pp == b) *pp = b->hash_next;
    b->hash_next = NULL;
}

/* Insert buf into the hash chain */
static void hash_insert(bcache_buf_t *b) {
    uint32_t h = hash_key(b->dev_id, b->lba);
    b->hash_next  = g_hash[h];
    g_hash[h]     = b;
}

/* Synchronously write a dirty buffer back to its device */
static void writeback(bcache_buf_t *b) {
    blkdev_t *dev = blkdev_get(b->dev_id);
    if (!dev) {
        KLOG_WARN("bcache: writeback: device id=%llu gone\n",
                  (unsigned long long)b->dev_id);
        b->dirty = false;
        return;
    }
    if (blkdev_write(dev, b->lba, 1, b->data) < 0)
        KLOG_WARN("bcache: writeback failed dev='%s' lba=%llu\n",
                  dev->name, (unsigned long long)b->lba);
    else
        b->dirty = false;
    g_stat_dirty++;
}

/* Find a free or evictable buffer; writes back dirty if needed */
static bcache_buf_t *evict_one(void) {
    /* Walk LRU from tail looking for refcount==0 */
    bcache_buf_t *victim = g_lru_tail;
    while (victim) {
        if (victim->refcount == 0) break;
        victim = victim->lru_prev;
    }
    if (!victim) return NULL; /* all pinned */

    if (victim->dirty) writeback(victim);
    hash_remove(victim);
    lru_remove(victim);
    g_stat_evicts++;
    victim->valid   = false;
    victim->dirty   = false;
    victim->dev_id  = 0;
    victim->lba     = 0;
    return victim;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
void bcache_init(void) {
    memset(g_pool, 0, sizeof(g_pool));
    memset(g_hash, 0, sizeof(g_hash));
    g_lru_head = g_lru_tail = NULL;

    /* Build initial free LRU chain (all unpinned, invalid) */
    for (int i = 0; i < BCACHE_NUM_BUFS; i++)
        lru_push_front(&g_pool[i]);

    KLOG_INFO("bcache: initialised — %d x %d B sectors = %d KiB pool\n",
              BCACHE_NUM_BUFS, BCACHE_SECTOR_SIZE,
              (BCACHE_NUM_BUFS * BCACHE_SECTOR_SIZE) / 1024);
}

bcache_buf_t *bcache_get(blkdev_t *dev, uint64_t lba) {
    uint32_t h = hash_key(dev->dev_id, lba);

    /* 1. Cache hit? */
    for (bcache_buf_t *b = g_hash[h]; b; b = b->hash_next) {
        if (b->dev_id == dev->dev_id && b->lba == lba && b->valid) {
            b->refcount++;
            lru_remove(b);
            lru_push_front(b);
            g_stat_hits++;
            return b;
        }
    }

    /* 2. Cache miss — find a free/evictable buffer */
    g_stat_misses++;
    bcache_buf_t *b = evict_one();
    if (!b) {
        KLOG_ERR("bcache: all %d buffers pinned — I/O deadlock!\n",
                 BCACHE_NUM_BUFS);
        return NULL;
    }

    /* 3. Fill buffer from device */
    if (blkdev_read(dev, lba, 1, b->data) < 0) {
        KLOG_WARN("bcache: read error dev='%s' lba=%llu\n",
                  dev->name, (unsigned long long)lba);
        lru_push_front(b); /* return as free */
        return NULL;
    }

    b->dev_id   = dev->dev_id;
    b->lba      = lba;
    b->valid    = true;
    b->dirty    = false;
    b->refcount = 1;
    hash_insert(b);
    lru_push_front(b);
    return b;
}

void bcache_mark_dirty(bcache_buf_t *buf) {
    if (buf) buf->dirty = true;
}

void bcache_release(bcache_buf_t *buf) {
    if (!buf) return;
    if (buf->refcount > 0) buf->refcount--;
    /* Write-back on release for simplicity (write-through model) */
    if (buf->refcount == 0 && buf->dirty)
        writeback(buf);
}

void bcache_flush_dev(blkdev_t *dev) {
    for (int i = 0; i < BCACHE_NUM_BUFS; i++) {
        bcache_buf_t *b = &g_pool[i];
        if (b->valid && b->dirty && b->dev_id == dev->dev_id)
            writeback(b);
    }
    blkdev_flush(dev);
}

void bcache_sync_all(void) {
    for (int i = 0; i < BCACHE_NUM_BUFS; i++) {
        bcache_buf_t *b = &g_pool[i];
        if (b->valid && b->dirty) writeback(b);
    }
}

void bcache_invalidate_dev(blkdev_t *dev) {
    for (int i = 0; i < BCACHE_NUM_BUFS; i++) {
        bcache_buf_t *b = &g_pool[i];
        if (b->valid && b->dev_id == dev->dev_id) {
            if (b->dirty) writeback(b);
            hash_remove(b);
            lru_remove(b);
            b->valid    = false;
            b->dirty    = false;
            b->refcount = 0;
            lru_push_front(b);
        }
    }
}

void bcache_print_stats(void) {
    KLOG_INFO("bcache: hits=%llu misses=%llu evicts=%llu writebacks=%llu\n",
              (unsigned long long)g_stat_hits,
              (unsigned long long)g_stat_misses,
              (unsigned long long)g_stat_evicts,
              (unsigned long long)g_stat_dirty);
}
