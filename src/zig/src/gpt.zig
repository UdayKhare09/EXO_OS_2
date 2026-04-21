//! src/gpt.zig — GPT partition table parser (Zig rewrite of fs/gpt.c)
//!
//! Implements the same C ABI as fs/gpt.h so it links drop-in.
//! Rules:
//!   * No stdlib — freestanding kernel code.
//!   * All C-visible symbols use `export`.
//!   * No heap allocation; the caller supplies the output array.

const std = @import("std");

// ── Extern C kernel functions ────────────────────────────────────────────────

/// Opaque handle to a block device (blkdev_t * in C).
pub const BlkDev = opaque {};

// blkdev_read(dev, lba, count, buf) → 0 on success, -1 on error
extern fn blkdev_read(dev: *BlkDev, lba: u64, count: u32, buf: [*]u8) c_int;

// blkdev_partition_create(parent, name, lba_start, lba_end) → blkdev_t* or NULL
// NOTE: lba_end is INCLUSIVE — matches the C signature in blkdev.h
extern fn blkdev_partition_create(
    parent:    *BlkDev,
    name:      [*:0]const u8,
    lba_start: u64,
    lba_end:   u64,
) ?*BlkDev;

// Return the ASCII name of a blkdev (the `name[32]` field).
// We declare this as an extern instead of dereferencing the opaque type.
extern fn blkdev_get_name(dev: *BlkDev) [*:0]const u8;

// Kernel logger shim — level 2 = INFO, 1 = WARN
extern fn klog_write_str(level: c_int, msg: [*:0]const u8) void;

// ── Sector I/O ───────────────────────────────────────────────────────────────
const SECTOR: usize = 512;

fn read_lba(dev: *BlkDev, lba: u64, buf: *[SECTOR]u8) bool {
    return blkdev_read(dev, lba, 1, buf) == 0;
}

// ── On-disk GPT structures (packed, little-endian host byte order) ───────────

pub const Guid = extern struct {
    data1: u32,
    data2: u16,
    data3: u16,
    data4: [8]u8,

    pub fn eql(a: *const Guid, b: *const Guid) bool {
        return a.data1 == b.data1 and
               a.data2 == b.data2 and
               a.data3 == b.data3 and
               std.mem.eql(u8, &a.data4, &b.data4);
    }

    pub fn is_zero(g: *const Guid) bool {
        return g.data1 == 0 and g.data2 == 0 and g.data3 == 0 and
               std.mem.allEqual(u8, &g.data4, 0);
    }
};
comptime { std.debug.assert(@sizeOf(Guid) == 16); }

/// GPT header at LBA 1 (first 92 bytes; rest of sector is padding).
const GptHeader = extern struct {
    signature:        [8]u8,    // "EFI PART"
    revision:         u32,
    header_size:      u32,
    header_crc32:     u32,
    _reserved:        u32,
    current_lba:      u64,
    backup_lba:       u64,
    first_usable:     u64,
    last_usable:      u64,
    disk_guid:        Guid,
    part_entry_lba:   u64,
    num_entries:      u32,
    entry_size:       u32,
    entries_crc32:    u32,

    fn valid(h: *const GptHeader) bool {
        return std.mem.eql(u8, &h.signature, "EFI PART");
    }
};

/// One 128-byte GPT partition entry.
const GptEntry = extern struct {
    type_guid:  Guid,
    part_guid:  Guid,
    start_lba:  u64,
    end_lba:    u64,
    attributes: u64,
    name:       [36]u16,  // UTF-16LE partition label
};
comptime { std.debug.assert(@sizeOf(GptEntry) == 128); }

// ── C-facing output struct — must match gpt_partition_t in gpt.h exactly ─────
pub const GptPartition = extern struct {
    type_guid:   Guid,
    part_guid:   Guid,
    lba_start:   u64,
    lba_end:     u64,
    flags:       u64,
    label:       [36]u16,
    label_ascii: [37]u8,
    blkdev:      ?*BlkDev,
};

// ── Well-known type GUIDs ────────────────────────────────────────────────────
export const GPT_GUID_EMPTY = Guid{
    .data1 = 0, .data2 = 0, .data3 = 0, .data4 = .{0} ** 8,
};
export const GPT_GUID_EFI_SYSTEM = Guid{
    .data1 = 0xC12A7328, .data2 = 0xF81F, .data3 = 0x11D2,
    .data4 = .{ 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B },
};
export const GPT_GUID_MS_BASIC_DATA = Guid{
    .data1 = 0xEBD0A0A2, .data2 = 0xB9E5, .data3 = 0x4433,
    .data4 = .{ 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 },
};
export const GPT_GUID_LINUX_DATA = Guid{
    .data1 = 0x0FC63DAF, .data2 = 0x8483, .data3 = 0x4772,
    .data4 = .{ 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 },
};
export const GPT_GUID_LINUX_SWAP = Guid{
    .data1 = 0x0657FD6D, .data2 = 0xA4AB, .data3 = 0x43C4,
    .data4 = .{ 0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F },
};

// ── Helpers ──────────────────────────────────────────────────────────────────

/// Lossy UTF-16LE → ASCII: replace non-ASCII code units with '?'.
fn utf16le_to_ascii(src: []const u16, dst: []u8) void {
    var i: usize = 0;
    while (i < src.len and i < dst.len - 1) : (i += 1) {
        const cp = src[i];
        if (cp == 0) break;
        dst[i] = if (cp < 0x80) @truncate(cp) else '?';
    }
    dst[i] = 0;
}

/// Build "parentN" device name into `buf`. Returns NUL-terminated slice.
fn fmt_part_name(parent_name: [*:0]const u8, idx: usize, buf: []u8) [:0]u8 {
    var pos: usize = 0;
    // Copy parent name
    var p: usize = 0;
    while (parent_name[p] != 0 and pos < buf.len - 4) : (p += 1) {
        buf[pos] = parent_name[p];
        pos += 1;
    }
    // Append 1-based decimal index
    var n: usize = idx + 1;
    var tmp: [8]u8 = undefined;
    var d: usize = 0;
    if (n == 0) { tmp[d] = '0'; d += 1; }
    while (n > 0) : (n /= 10) {
        tmp[d] = '0' + @as(u8, @truncate(n % 10));
        d += 1;
    }
    while (d > 0) {
        d -= 1;
        if (pos < buf.len - 1) { buf[pos] = tmp[d]; pos += 1; }
    }
    buf[pos] = 0;
    return buf[0..pos :0];
}

// ── Exported C ABI ───────────────────────────────────────────────────────────

/// bool gpt_guid_equal(const gpt_guid_t *a, const gpt_guid_t *b)
export fn gpt_guid_equal(a: *const Guid, b: *const Guid) bool {
    return a.eql(b);
}

/// const char *gpt_guid_type_str(const gpt_guid_t *g)
export fn gpt_guid_type_str(g: *const Guid) [*:0]const u8 {
    if (g.eql(&GPT_GUID_EFI_SYSTEM))    return "EFI System";
    if (g.eql(&GPT_GUID_MS_BASIC_DATA)) return "Microsoft Basic Data";
    if (g.eql(&GPT_GUID_LINUX_DATA))    return "Linux Filesystem";
    if (g.eql(&GPT_GUID_LINUX_SWAP))    return "Linux Swap";
    if (g.is_zero())                     return "Empty";
    return "Unknown";
}

/// int gpt_scan(blkdev_t *dev, gpt_partition_t *out, int max)
///
/// Reads LBA 1 for the GPT header, iterates partition entries, populates
/// `out[]`, and calls blkdev_partition_create() for each valid entry.
/// Returns the count of partitions found (0 on error / empty disk).
export fn gpt_scan(dev: *BlkDev, out: [*]GptPartition, max: c_int) c_int {
    if (max <= 0) return 0;

    // ── Read GPT header at LBA 1 ─────────────────────────────────────────
    var hdr_sector: [SECTOR]u8 align(8) = undefined;
    if (!read_lba(dev, 1, &hdr_sector)) {
        klog_write_str(1, "gpt(zig): failed to read LBA 1\n");
        return 0;
    }

    const hdr = @as(*const GptHeader, @ptrCast(@alignCast(&hdr_sector)));
    if (!hdr.valid()) {
        klog_write_str(1, "gpt(zig): invalid GPT signature\n");
        return 0;
    }

    const entry_size = hdr.entry_size;
    if (entry_size == 0 or entry_size > SECTOR) {
        klog_write_str(1, "gpt(zig): unsupported partition entry size\n");
        return 0;
    }

    const n_want:           u32 = hdr.num_entries;
    const entries_per_sec:  u32 = @intCast(SECTOR / entry_size);
    const part_lba:         u64 = hdr.part_entry_lba;
    const parent_name:  [*:0]const u8 = blkdev_get_name(dev);

    // ── Iterate entries ──────────────────────────────────────────────────
    var found: c_int = 0;
    var name_buf: [32]u8 = undefined;

    var ei: u32 = 0;
    while (ei < n_want and found < max) : (ei += 1) {
        const sec_idx:  u64 = ei / entries_per_sec;
        const slot:     u32 = ei % entries_per_sec;

        var ent_buf: [SECTOR]u8 align(8) = undefined;
        if (!read_lba(dev, part_lba + sec_idx, &ent_buf)) break;

        const byte_off: usize = @as(usize, slot) * entry_size;
        // Safety: ent_buf is align(8); GptEntry is 128 bytes; entry_size >= 128
        const ent = @as(*const GptEntry, @ptrCast(@alignCast(&ent_buf[byte_off])));

        // Skip empty entries
        if (ent.type_guid.is_zero()) continue;
        if (ent.start_lba == 0 or ent.end_lba < ent.start_lba) continue;

        // Fill output
        const slot_out = &out[@intCast(found)];
        slot_out.type_guid = ent.type_guid;
        slot_out.part_guid = ent.part_guid;
        slot_out.lba_start = ent.start_lba;
        slot_out.lba_end   = ent.end_lba;
        slot_out.flags     = ent.attributes;
        slot_out.label     = ent.name;
        utf16le_to_ascii(&ent.name, &slot_out.label_ascii);
        slot_out.blkdev    = null;

        // Register partition blkdev
        const pname = fmt_part_name(parent_name, @intCast(found), &name_buf);
        slot_out.blkdev = blkdev_partition_create(
            dev, pname.ptr, ent.start_lba, ent.end_lba,
        );

        found += 1;
    }

    return found;
}

// ── Panic handler (mandatory for freestanding Zig) ───────────────────────────
pub fn panic(_: []const u8, _: ?*std.builtin.StackTrace, _: ?usize) noreturn {
    while (true) {
        asm volatile ("cli");
        asm volatile ("hlt");
    }
}
