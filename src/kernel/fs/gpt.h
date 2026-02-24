/* fs/gpt.h — GUID Partition Table (GPT) parser
 *
 * Reads the GPT on a raw block device, creates blkdev_t partition wrappers
 * for each valid entry, and registers them with the blkdev layer.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "drivers/storage/blkdev.h"

/* ── GUID (16-byte little-endian) ───────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} gpt_guid_t;

/* ── Well-known partition type GUIDs ─────────────────────────────────────── */
/* Unused / empty entry */
extern const gpt_guid_t GPT_GUID_EMPTY;
/* EFI System Partition */
extern const gpt_guid_t GPT_GUID_EFI_SYSTEM;
/* Microsoft Basic Data */
extern const gpt_guid_t GPT_GUID_MS_BASIC_DATA;
/* Linux filesystem data (ext2/3/4, xfs, etc.) */
extern const gpt_guid_t GPT_GUID_LINUX_DATA;
/* Linux swap */
extern const gpt_guid_t GPT_GUID_LINUX_SWAP;

/* ── Partition info (one per discovered partition) ───────────────────────── */
typedef struct {
    gpt_guid_t type_guid;   /* partition type GUID                           */
    gpt_guid_t part_guid;   /* unique partition GUID                         */
    uint64_t   lba_start;   /* first sector (inclusive)                      */
    uint64_t   lba_end;     /* last sector (inclusive)                       */
    uint64_t   flags;
    uint16_t   label[36];   /* UTF-16LE partition name                       */
    char       label_ascii[37]; /* ASCII copy (lossy) for logging            */
    blkdev_t  *blkdev;      /* registered partition blkdev_t (post-scan)     */
} gpt_partition_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Scan the GPT on `dev`. Populates `out[0..max-1]`.
 * Automatically creates and registers blkdev_t wrappers for each valid entry,
 * named "<parent_name><N>" (e.g. "vda1", "sda2").
 * Returns number of partitions found (0 on error or empty disk). */
int gpt_scan(blkdev_t *dev, gpt_partition_t *out, int max);

/* Compare two GUIDs. Returns true if equal. */
bool gpt_guid_equal(const gpt_guid_t *a, const gpt_guid_t *b);

/* ASCII description of a known GUID type (for logging). */
const char *gpt_guid_type_str(const gpt_guid_t *g);
