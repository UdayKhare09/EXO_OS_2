/* drivers/storage/blkdev.c — Block device registry */
#include "blkdev.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

/* ── Global registry ─────────────────────────────────────────────────────── */
static blkdev_t *g_blkdevs[BLKDEV_MAX];
static int       g_blkdev_count = 0;
static uint64_t  g_next_dev_id  = 1;

/* ── Registry API ────────────────────────────────────────────────────────── */
int blkdev_register(blkdev_t *dev) {
    if (g_blkdev_count >= BLKDEV_MAX) {
        KLOG_WARN("blkdev: registry full, cannot register '%s'\n", dev->name);
        return -1;
    }

    dev->dev_id = g_next_dev_id++;

    /* Cache geometry from ops */
    if (dev->ops->get_block_size)
        dev->block_size = dev->ops->get_block_size(dev);
    else
        dev->block_size = 512;

    if (dev->ops->get_block_count)
        dev->block_count = dev->ops->get_block_count(dev);
    else
        dev->block_count = 0;

    g_blkdevs[g_blkdev_count++] = dev;

    KLOG_INFO("blkdev: registered '%s' (id=%llu) %llu sectors @ %u B\n",
              dev->name, (unsigned long long)dev->dev_id,
              (unsigned long long)dev->block_count, dev->block_size);
    return 0;
}

blkdev_t *blkdev_get(uint64_t dev_id) {
    for (int i = 0; i < g_blkdev_count; i++)
        if (g_blkdevs[i]->dev_id == dev_id)
            return g_blkdevs[i];
    return NULL;
}

blkdev_t *blkdev_find_by_name(const char *name) {
    for (int i = 0; i < g_blkdev_count; i++)
        if (strcmp(g_blkdevs[i]->name, name) == 0)
            return g_blkdevs[i];
    return NULL;
}

int blkdev_count(void) {
    return g_blkdev_count;
}

blkdev_t *blkdev_get_nth(int n) {
    if (n < 0 || n >= g_blkdev_count) return NULL;
    return g_blkdevs[n];
}

/* ── Direct I/O (no cache) ───────────────────────────────────────────────── */
int blkdev_read(blkdev_t *dev, uint64_t lba, uint32_t count, void *buf) {
    if (!dev || !dev->ops->read_blocks) return -1;
    return dev->ops->read_blocks(dev, lba, count, buf);
}

int blkdev_write(blkdev_t *dev, uint64_t lba, uint32_t count,
                 const void *buf) {
    if (!dev || !dev->ops->write_blocks) return -1;
    return dev->ops->write_blocks(dev, lba, count, buf);
}

int blkdev_flush(blkdev_t *dev) {
    if (!dev) return -1;
    if (dev->ops->flush) return dev->ops->flush(dev);
    return 0; /* no flush needed */
}

/* ── Partition wrapper implementation ───────────────────────────────────── */
typedef struct {
    blkdev_t *parent;
    uint64_t  lba_start;
    uint64_t  lba_end;         /* inclusive */
} part_priv_t;

static int part_read(blkdev_t *dev, uint64_t lba, uint32_t count, void *buf) {
    part_priv_t *p = (part_priv_t *)dev->priv;
    uint64_t abs_lba = p->lba_start + lba;
    /* Bounds check */
    if (abs_lba + count - 1 > p->lba_end) return -1;
    return blkdev_read(p->parent, abs_lba, count, buf);
}

static int part_write(blkdev_t *dev, uint64_t lba, uint32_t count,
                      const void *buf) {
    part_priv_t *p = (part_priv_t *)dev->priv;
    uint64_t abs_lba = p->lba_start + lba;
    if (abs_lba + count - 1 > p->lba_end) return -1;
    return blkdev_write(p->parent, abs_lba, count, buf);
}

static uint64_t part_get_block_count(blkdev_t *dev) {
    part_priv_t *p = (part_priv_t *)dev->priv;
    return p->lba_end - p->lba_start + 1;
}

static uint32_t part_get_block_size(blkdev_t *dev) {
    part_priv_t *p = (part_priv_t *)dev->priv;
    return p->parent->block_size;
}

static int part_flush(blkdev_t *dev) {
    part_priv_t *p = (part_priv_t *)dev->priv;
    return blkdev_flush(p->parent);
}

static blkdev_ops_t g_part_ops = {
    .read_blocks     = part_read,
    .write_blocks    = part_write,
    .get_block_count = part_get_block_count,
    .get_block_size  = part_get_block_size,
    .flush           = part_flush,
};

blkdev_t *blkdev_partition_create(blkdev_t *parent, const char *name,
                                   uint64_t lba_start, uint64_t lba_end) {
    blkdev_t    *dev  = kzalloc(sizeof(blkdev_t));
    part_priv_t *priv = kzalloc(sizeof(part_priv_t));
    if (!dev || !priv) {
        kfree(dev);
        kfree(priv);
        return NULL;
    }

    strncpy(dev->name, name, sizeof(dev->name) - 1);
    dev->ops         = &g_part_ops;
    dev->priv        = priv;
    dev->part_offset = lba_start;

    priv->parent    = parent;
    priv->lba_start = lba_start;
    priv->lba_end   = lba_end;

    if (blkdev_register(dev) < 0) {
        kfree(dev);
        kfree(priv);
        return NULL;
    }
    return dev;
}

/* ── Language-addon helpers ──────────────────────────────────────────────── */
/* Called by the Zig addon (libexo_zig.a) to read dev->name without exposing
 * the full blkdev_t layout through an opaque pointer. */
const char *blkdev_get_name(const blkdev_t *dev) {
    return dev ? dev->name : "";
}
