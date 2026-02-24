/* gfx/font.h — Kernel font interface (pre-rasterized at build time) */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Atlas produced by tools/gen_font at build time */
typedef struct {
    int            cell_w;      /* glyph cell width  in pixels */
    int            cell_h;      /* glyph cell height in pixels */
    int            first_char;  /* first codepoint in atlas    */
    int            num_chars;   /* number of entries           */
    const uint8_t *bitmaps;     /* [num_chars][cell_w*cell_h]  */
} gfx_font_atlas_t;

/* Exported from the generated gfx_font_data.c */
extern const gfx_font_atlas_t g_font_atlas;

/* Return the alpha-coverage bitmap for codepoint c (NULL if out of range) */
static inline const uint8_t *font_get_glyph(int c) {
    const gfx_font_atlas_t *a = &g_font_atlas;
    if (c < a->first_char || c >= a->first_char + a->num_chars) return NULL;
    int stride = a->cell_w * a->cell_h;
    return a->bitmaps + (c - a->first_char) * stride;
}
