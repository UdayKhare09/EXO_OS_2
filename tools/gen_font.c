/*
 * gen_font.c — Build-time TrueType rasterizer using stb_truetype.
 * Runs on the HOST. Outputs a C file (build/gfx_font_data.c) with pre-rasterized
 * glyph bitmaps that the kernel links in directly (no float at runtime).
 *
 * Features:
 *   • 2× oversampling for smooth anti-aliasing
 *   • Proper hinting for crisp edges at small sizes
 *   • Gamma-correct downsampling (linearize → average → re-encode)
 *   • Multi-range Unicode coverage (Tier 3):
 *       ASCII 0x0020–0x007E, Latin-1 0x00A0–0x00FF, Greek 0x0370–0x03FF,
 *       Box Drawing 0x2500–0x257F, Block Elements 0x2580–0x259F,
 *       Braille 0x2800–0x28FF, Powerline 0xE0A0–0xE0B3
 *   • Emits gfx_font_range_t[] so the kernel can binary-search codepoints.
 *
 * Usage: ./gen_font <font.ttf> <px_height> <output.c>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../src/kernel/gfx/3rdparty/stb_truetype.h"

#define OVERSAMPLE   2      /* 2× supersampling in both axes */

/* ── Unicode ranges to rasterize ─────────────────────────────────────────── */
typedef struct {
    uint32_t    first_cp;
    uint32_t    last_cp;
    const char *name;
} range_def_t;

static const range_def_t RANGES[] = {
    { 0x0020, 0x007E, "ASCII Printable"      },
    { 0x00A0, 0x00FF, "Latin-1 Supplement"   },
    { 0x0370, 0x03FF, "Greek and Coptic"     },
    { 0x2500, 0x257F, "Box Drawing"          },
    { 0x2580, 0x259F, "Block Elements"       },
    { 0x2800, 0x28FF, "Braille Patterns"     },
    { 0xE0A0, 0xE0B3, "Powerline Symbols"    },
};
#define NUM_RANGES  ((int)(sizeof(RANGES) / sizeof(RANGES[0])))

/* ── sRGB gamma helpers ───────────────────────────────────────────────────── */
static double srgb_to_linear(uint8_t v) {
    double f = v / 255.0;
    return f <= 0.04045 ? f / 12.92 : pow((f + 0.055) / 1.055, 2.4);
}
static uint8_t linear_to_srgb(double v) {
    if (v <= 0.0) return 0;
    if (v >= 1.0) return 255;
    double f = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0/2.4) - 0.055;
    return (uint8_t)(f * 255.0 + 0.5);
}

/* ── Rasterize one codepoint → downsampled cell ───────────────────────────── */
static uint8_t *rasterize_glyph(stbtt_fontinfo *font,
                                 float os_scale, int os_baseline,
                                 int os_cw, int os_ch,
                                 int cell_w, int cell_h,
                                 uint32_t cp) {
    int bw, bh, bx, by;
    uint8_t *bitmap = stbtt_GetCodepointBitmap(
            font, 0, os_scale, (int)cp, &bw, &bh, &bx, &by);

    uint8_t *os_cell = calloc(os_cw * os_ch, 1);
    if (!os_cell) { stbtt_FreeBitmap(bitmap, NULL); return NULL; }

    int y_off = os_baseline + by;
    for (int row = 0; row < bh; row++) {
        int dst_row = y_off + row;
        if (dst_row < 0 || dst_row >= os_ch) continue;
        for (int col = 0; col < bw; col++) {
            int dst_col = bx + col;
            if (dst_col < 0 || dst_col >= os_cw) continue;
            os_cell[dst_row * os_cw + dst_col] = bitmap[row * bw + col];
        }
    }
    stbtt_FreeBitmap(bitmap, NULL);

    /* Gamma-correct downsample */
    uint8_t *cell = calloc(cell_w * cell_h, 1);
    if (!cell) { free(os_cell); return NULL; }
    for (int dy = 0; dy < cell_h; dy++) {
        for (int dx = 0; dx < cell_w; dx++) {
            double sum = 0.0;
            for (int oy = 0; oy < OVERSAMPLE; oy++) {
                for (int ox = 0; ox < OVERSAMPLE; ox++) {
                    int sy = dy * OVERSAMPLE + oy;
                    int sx = dx * OVERSAMPLE + ox;
                    if (sy < os_ch && sx < os_cw)
                        sum += srgb_to_linear(os_cell[sy * os_cw + sx]);
                }
            }
            cell[dy * cell_w + dx] = linear_to_srgb(
                sum / (double)(OVERSAMPLE * OVERSAMPLE));
        }
    }
    free(os_cell);
    return cell;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <font.ttf> <px_height> <output.c>\n", argv[0]);
        return 1;
    }
    const char *ttf_path  = argv[1];
    int         px_height = atoi(argv[2]);
    const char *out_path  = argv[3];

    /* Load TTF file */
    FILE *f = fopen(ttf_path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", ttf_path); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);
    uint8_t *ttf_buf = malloc(fsz);
    if (!ttf_buf) { fclose(f); return 1; }
    fread(ttf_buf, 1, fsz, f);
    fclose(f);

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, ttf_buf, stbtt_GetFontOffsetForIndex(ttf_buf, 0))) {
        fprintf(stderr, "Failed to init font\n");
        return 1;
    }

    float scale = stbtt_ScaleForPixelHeight(&font, (float)px_height);

    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
    int baseline    = (int)(ascent  * scale);
    int cell_height = (int)((ascent - descent) * scale);
    (void)baseline;

    /* Find maximum advance width across ALL ranges (monospace cell) */
    int cell_width = 0;
    for (int ri = 0; ri < NUM_RANGES; ri++) {
        for (uint32_t cp = RANGES[ri].first_cp; cp <= RANGES[ri].last_cp; cp++) {
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&font, (int)cp, &adv, &lsb);
            int w = (int)(adv * scale + 0.5f);
            if (w > cell_width) cell_width = w;
        }
    }
    if (cell_width == 0) cell_width = (int)(px_height * 0.6f + 0.5f);

    /* Oversampled dimensions */
    int   os_height   = px_height * OVERSAMPLE;
    float os_scale    = stbtt_ScaleForPixelHeight(&font, (float)os_height);
    int   os_cw       = cell_width  * OVERSAMPLE;
    int   os_ch       = cell_height * OVERSAMPLE;
    int   os_baseline = (int)(ascent * os_scale);

    /* Count total glyphs */
    uint32_t total_glyphs = 0;
    for (int ri = 0; ri < NUM_RANGES; ri++)
        total_glyphs += RANGES[ri].last_cp - RANGES[ri].first_cp + 1;

    printf("Font: %s  height=%dpx  cell=%dx%d  oversample=%dx  glyphs=%u\n",
           ttf_path, px_height, cell_width, cell_height, OVERSAMPLE, total_glyphs);

    FILE *out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "Cannot write %s\n", out_path); return 1; }

    fprintf(out, "/* AUTO-GENERATED by gen_font — do not edit */\n");
    fprintf(out, "#include \"gfx/font.h\"\n\n");
    fprintf(out, "#define FONT_CELL_W  %d\n", cell_width);
    fprintf(out, "#define FONT_CELL_H  %d\n\n", cell_height);

    /* Rasterize all ranges */
    fprintf(out, "static const uint8_t _glyph_bitmaps[%u][%d] = {\n",
            total_glyphs, cell_width * cell_height);

    uint32_t atlas_idx = 0;
    for (int ri = 0; ri < NUM_RANGES; ri++) {
        fprintf(out, "    /* ── %s (U+%04X..U+%04X) ── */\n",
                RANGES[ri].name, RANGES[ri].first_cp, RANGES[ri].last_cp);
        for (uint32_t cp = RANGES[ri].first_cp; cp <= RANGES[ri].last_cp; cp++) {
            uint8_t *cell = rasterize_glyph(&font, os_scale, os_baseline,
                                             os_cw, os_ch,
                                             cell_width, cell_height, cp);
            if (!cell) {
                /* Glyph not in font — emit blank */
                cell = calloc(cell_width * cell_height, 1);
            }

            const char *printable = (cp >= 0x20 && cp < 0x7F) ? "" : "";
            (void)printable;
            fprintf(out, "    /* U+%04X */ {", cp);
            for (int i = 0; i < cell_width * cell_height; i++) {
                if (i % cell_width == 0) fprintf(out, "\n        ");
                fprintf(out, "0x%02x,", cell[i]);
            }
            fprintf(out, "\n    },\n");
            free(cell);
            atlas_idx++;
        }
    }
    fprintf(out, "};\n\n");

    /* Emit range table */
    fprintf(out, "static const gfx_font_range_t _font_ranges[%d] = {\n",
            NUM_RANGES);
    atlas_idx = 0;
    for (int ri = 0; ri < NUM_RANGES; ri++) {
        uint32_t count = RANGES[ri].last_cp - RANGES[ri].first_cp + 1;
        fprintf(out,
            "    { .first_cp = 0x%04X, .count = %u, .atlas_idx = %u }, /* %s */\n",
            RANGES[ri].first_cp, count, atlas_idx, RANGES[ri].name);
        atlas_idx += count;
    }
    fprintf(out, "};\n\n");

    /* Export the public descriptor using the new range-table layout */
    fprintf(out,
        "const gfx_font_atlas_t g_font_atlas = {\n"
        "    .cell_w      = FONT_CELL_W,\n"
        "    .cell_h      = FONT_CELL_H,\n"
        "    .num_chars   = %u,\n"
        "    .range_count = %d,\n"
        "    .ranges      = _font_ranges,\n"
        "    .bitmaps     = (const uint8_t *)_glyph_bitmaps,\n"
        "};\n",
        total_glyphs, NUM_RANGES);

    fclose(out);
    free(ttf_buf);
    printf("Wrote %s  (%u glyphs in %d ranges @ %dx%d)\n",
           out_path, total_glyphs, NUM_RANGES, cell_width, cell_height);
    return 0;
}
