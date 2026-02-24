/* fs/gpt.c — GUID Partition Table parser */
#include "gpt.h"
#include "drivers/storage/blkdev.h"
#include "lib/klog.h"
#include "lib/string.h"
#include "mm/kmalloc.h"
#include <stdarg.h>

#include <stdint.h>
#include <stdbool.h>

/* ── CRC-32 (IEEE 802.3 polynomial) ─────────────────────────────────────── */
static uint32_t g_crc32_table[256];
static bool     g_crc32_init = false;

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        g_crc32_table[i] = c;
    }
    g_crc32_init = true;
}

static uint32_t crc32(const uint8_t *data, size_t len) {
    if (!g_crc32_init) crc32_init_table();
    uint32_t c = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
        c = g_crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFU;
}

/* ── GPT on-disk structures ──────────────────────────────────────────────── */
#define GPT_HEADER_SIGNATURE  0x5452415020494645ULL  /* "EFI PART" LE       */
#define GPT_HEADER_REVISION   0x00010000U

typedef struct __attribute__((packed)) {
    uint64_t signature;        /* "EFI PART"                                 */
    uint32_t revision;
    uint32_t header_size;      /* usually 92                                 */
    uint32_t header_crc32;     /* CRC of this header (field zeroed)          */
    uint32_t reserved;
    uint64_t my_lba;           /* LBA of this header (1 for primary)         */
    uint64_t alternate_lba;    /* LBA of backup header                       */
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    gpt_guid_t disk_guid;
    uint64_t part_entry_start_lba;
    uint32_t num_part_entries;
    uint32_t part_entry_size;  /* usually 128                                */
    uint32_t part_array_crc32;
    /* rest padded to sector */
} gpt_header_t;

typedef struct __attribute__((packed)) {
    gpt_guid_t type_guid;
    gpt_guid_t part_guid;
    uint64_t   lba_start;
    uint64_t   lba_end;
    uint64_t   flags;
    uint16_t   name[36];   /* UTF-16LE                                       */
} gpt_entry_t;

/* ── Well-known GUIDs ────────────────────────────────────────────────────── */
const gpt_guid_t GPT_GUID_EMPTY = {
    0x00000000, 0x0000, 0x0000,
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
};
const gpt_guid_t GPT_GUID_EFI_SYSTEM = {
    0xC12A7328, 0xF81F, 0x11D2,
    { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }
};
const gpt_guid_t GPT_GUID_MS_BASIC_DATA = {
    0xEBD0A0A2, 0xB9E5, 0x4433,
    { 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 }
};
const gpt_guid_t GPT_GUID_LINUX_DATA = {
    0x0FC63DAF, 0x8483, 0x4772,
    { 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 }
};
const gpt_guid_t GPT_GUID_LINUX_SWAP = {
    0x0657FD6D, 0xA4AB, 0x43C4,
    { 0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F }
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */
bool gpt_guid_equal(const gpt_guid_t *a, const gpt_guid_t *b) {
    return memcmp(a, b, sizeof(gpt_guid_t)) == 0;
}

const char *gpt_guid_type_str(const gpt_guid_t *g) {
    if (gpt_guid_equal(g, &GPT_GUID_EFI_SYSTEM))    return "EFI System";
    if (gpt_guid_equal(g, &GPT_GUID_MS_BASIC_DATA)) return "MS Basic Data";
    if (gpt_guid_equal(g, &GPT_GUID_LINUX_DATA))    return "Linux Data";
    if (gpt_guid_equal(g, &GPT_GUID_LINUX_SWAP))    return "Linux Swap";
    if (gpt_guid_equal(g, &GPT_GUID_EMPTY))         return "Empty";
    return "Unknown";
}

/* UTF-16LE → ASCII (lossy: non-ASCII replaced with '?') */
static void utf16le_to_ascii(const uint16_t *src, int max_chars, char *dst) {
    for (int i = 0; i < max_chars; i++) {
        uint16_t c = src[i];
        if (c == 0) { dst[i] = '\0'; return; }
        dst[i] = (c < 0x80) ? (char)c : '?';
    }
    dst[max_chars] = '\0';
}

/* ── Public API ──────────────────────────────────────────────────────────── */
int gpt_scan(blkdev_t *dev, gpt_partition_t *out, int max) {
    if (!dev || !out || max <= 0) return 0;

    /* ── Read LBA 1 (primary GPT header) ── */
    uint8_t sector[512];
    if (blkdev_read(dev, 1, 1, sector) < 0) {
        KLOG_WARN("gpt: '%s': failed to read LBA 1\n", dev->name);
        return 0;
    }

    gpt_header_t hdr;
    memcpy(&hdr, sector, sizeof(hdr));

    /* Validate signature */
    if (hdr.signature != GPT_HEADER_SIGNATURE) {
        KLOG_INFO("gpt: '%s': no GPT signature (not a GPT disk)\n", dev->name);
        return 0;
    }
    if (hdr.revision != GPT_HEADER_REVISION) {
        KLOG_WARN("gpt: '%s': unknown revision 0x%08X\n",
                  dev->name, hdr.revision);
    }

    /* Validate header CRC */
    uint32_t saved_crc = hdr.header_crc32;
    hdr.header_crc32   = 0;
    uint32_t computed  = crc32((uint8_t *)&hdr, hdr.header_size);
    if (computed != saved_crc) {
        KLOG_WARN("gpt: '%s': header CRC mismatch (got %08X expected %08X)\n",
                  dev->name, computed, saved_crc);
        return 0;
    }

    uint32_t nparts  = hdr.num_part_entries;
    uint32_t entry_sz = hdr.part_entry_size;
    uint64_t arr_lba  = hdr.part_entry_start_lba;

    KLOG_INFO("gpt: '%s': %u partition entries, entry_size=%u, arr_lba=%llu\n",
              dev->name, nparts, entry_sz,
              (unsigned long long)arr_lba);

    /* Validate entry size */
    if (entry_sz < sizeof(gpt_entry_t) || entry_sz > 512) {
        KLOG_WARN("gpt: '%s': unexpected entry size %u\n", dev->name, entry_sz);
        return 0;
    }

    /* How many entries fit in one sector */
    uint32_t eps = 512 / entry_sz;  /* entries per sector */
    int count = 0;

    /* Determine parent device name length for partition naming */
    int pname_len = (int)strlen(dev->name);
    /* If parent ends in a digit (e.g. "nvme0n1"), add 'p' separator */
    bool needs_p = (pname_len > 0 &&
                    dev->name[pname_len - 1] >= '0' &&
                    dev->name[pname_len - 1] <= '9');
    int part_num = 1;

    for (uint32_t i = 0; i < nparts && count < max; ) {
        uint64_t lba = arr_lba + i / eps;
        uint8_t  buf[512];
        if (blkdev_read(dev, lba, 1, buf) < 0) break;

        for (uint32_t j = 0; j < eps && i < nparts && count < max; j++, i++) {
            gpt_entry_t *e = (gpt_entry_t *)(buf + j * entry_sz);

            /* Skip empty entries */
            if (gpt_guid_equal(&e->type_guid, &GPT_GUID_EMPTY)) continue;
            if (e->lba_start == 0 && e->lba_end == 0) continue;

            gpt_partition_t *p = &out[count];
            p->type_guid = e->type_guid;
            p->part_guid = e->part_guid;
            p->lba_start = e->lba_start;
            p->lba_end   = e->lba_end;
            p->flags     = e->flags;
            memcpy(p->label, e->name, sizeof(p->label));
            utf16le_to_ascii(e->name, 36, p->label_ascii);

            /* Create partition blkdev name: e.g. "vda1", "sda1", "nvme0n1p1" */
            char part_name[40];
            if (needs_p)
                ksnprintf(part_name, sizeof(part_name), "%sp%d",
                          dev->name, part_num);
            else
                ksnprintf(part_name, sizeof(part_name), "%s%d",
                          dev->name, part_num);

            p->blkdev = blkdev_partition_create(dev, part_name,
                                                 e->lba_start, e->lba_end);

            KLOG_INFO("gpt: '%s' part%d '%s' [%llu..%llu] %s\n",
                      dev->name, part_num, p->label_ascii,
                      (unsigned long long)e->lba_start,
                      (unsigned long long)e->lba_end,
                      gpt_guid_type_str(&e->type_guid));

            count++;
            part_num++;
        }
    }

    KLOG_INFO("gpt: '%s': found %d valid partition(s)\n", dev->name, count);
    return count;
}
