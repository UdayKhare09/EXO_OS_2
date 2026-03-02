/* gfx/font.h — Kernel font interface (pre-rasterized at build time) */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* One contiguous block of Unicode codepoints present in the atlas */
typedef struct {
    uint32_t first_cp;  /* first Unicode codepoint in this range            */
    uint32_t count;     /* number of glyphs (consecutive codepoints)        */
    uint32_t atlas_idx; /* index of first glyph in bitmaps[]                */
} gfx_font_range_t;

/* Atlas produced by tools/gen_font at build time.
 * Ranges are sorted in ascending first_cp order (binary search).           */
typedef struct {
    int                     cell_w;       /* glyph cell width  in pixels    */
    int                     cell_h;       /* glyph cell height in pixels    */
    uint32_t                num_chars;    /* total glyph count              */
    uint32_t                range_count;  /* number of entries in ranges[]  */
    const gfx_font_range_t *ranges;       /* sorted range table             */
    const uint8_t          *bitmaps;      /* [num_chars][cell_w*cell_h]     */
} gfx_font_atlas_t;

/* Exported from the generated gfx_font_data.c */
extern const gfx_font_atlas_t g_font_atlas;

/* Return the alpha-coverage bitmap for Unicode codepoint cp.
 * Returns NULL if cp is not present in any range of the atlas.             */
static inline const uint8_t *font_get_glyph(uint32_t cp) {
    const gfx_font_atlas_t *a = &g_font_atlas;
    /* Binary search over sorted ranges */
    uint32_t lo = 0, hi = a->range_count;
    while (lo < hi) {
        uint32_t mid   = (lo + hi) >> 1;
        uint32_t first = a->ranges[mid].first_cp;
        uint32_t count = a->ranges[mid].count;
        if      (cp < first)          hi = mid;
        else if (cp >= first + count) lo = mid + 1;
        else {
            uint32_t idx   = a->ranges[mid].atlas_idx + (cp - first);
            uint32_t stride = (uint32_t)(a->cell_w * a->cell_h);
            return a->bitmaps + idx * stride;
        }
    }
    return NULL;
}
