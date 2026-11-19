// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui/gfx.h"

static const char extended[] = GLYPH_TR_G_BREVE_UPPER GLYPH_TR_G_BREVE_LOWER
    GLYPH_TR_I_DOTTED GLYPH_TR_I_DOTLESS GLYPH_TR_S_CEDILLA_UPPER GLYPH_TR_S_CEDILLA_LOWER;

#if CONFIG_FONT_TURKISH
static const char samples[] = "I" GLYPH_TR_S_CEDILLA_LOWER GLYPH_TR_I_DOTLESS "k  "
    "\xc7" "a" GLYPH_TR_G_BREVE_LOWER "r" GLYPH_TR_I_DOTLESS "  "
    "G\xf6r\xfc" GLYPH_TR_S_CEDILLA_LOWER "me";

static const uint8_t *ink(const font_t *font, uint8_t byte, int *top, int *rows)
{
    const uint8_t *bits = font_glyph(font, byte, top, rows);
    assert(bits != NULL && *top >= 0 && *rows > 0 && *top + *rows <= font->h);
    return bits;
}

static void check_font(const font_t *font)
{
    static const char latin[] = "\xc7\xe7\xd6\xf6\xdc\xfc";
    for (unsigned i = 0; i < sizeof(extended) - 1; ++i) {
        int top, rows, plain_top, plain_rows;
        unsigned stride = (font->w * font->bpp + 7u) / 8u;
        const uint8_t *bits = ink(font, (uint8_t)extended[i], &top, &rows);
        const uint8_t *plain = ink(font, (uint8_t)"GgIiSs"[i], &plain_top, &plain_rows);
        if (top == plain_top && rows == plain_rows && !memcmp(bits, plain, (size_t)rows * stride))
            fprintf(stderr, "%ux%u glyph 0x%02x matches ASCII %c\n", font->w, font->h,
                    (unsigned)(uint8_t)extended[i], "GgIiSs"[i]);
        assert(top != plain_top || rows != plain_rows || memcmp(bits, plain, (size_t)rows * stride));
        assert(font_fold(extended[i]) == "GgIiSs"[i]);

        bits = ink(font, (uint8_t)latin[i], &top, &rows);
        plain = ink(font, (uint8_t)"CcOoUu"[i], &plain_top, &plain_rows);
        if (top == plain_top && rows == plain_rows && !memcmp(bits, plain, (size_t)rows * stride))
            fprintf(stderr, "%ux%u glyph 0x%02x matches ASCII %c\n", font->w, font->h,
                    (unsigned)(uint8_t)latin[i], "CcOoUu"[i]);
        assert(top != plain_top || rows != plain_rows || memcmp(bits, plain, (size_t)rows * stride));
    }
    assert(gfx_text_width(font, samples) == (int)strlen(samples) * font->w);
    assert(gfx_text(font, 0, 0, samples, 0xffff) == gfx_text_width(font, samples));
    assert(gfx_text(font, 0, font->h, extended, 0xffff) == 6 * font->w);
    for (int i = 0; i < 6; ++i) {
        bool drawn = false;
        for (int y = font->h; y < 2 * font->h; ++y)
            for (int x = i * font->w; x < (i + 1) * font->w; ++x)
                drawn |= gfx_get_pixel(x, y) != 0;
        assert(drawn);
    }
}
#endif

int main(void)
{
    const font_t *faces[] = {&font_5x8, &font_aa_6x12, &font_aa_8x16,
        &font_aa_9x18, &font_aa_12x24, FONT_AA_LARGEST};
#if CONFIG_FONT_TURKISH
    const hal_display_t panel = {.width = 320, .height = 64, .bpp = 16};
    assert(gfx_init(&panel));
    for (unsigned i = 0; i < sizeof(faces) / sizeof(faces[0]); ++i) {
        gfx_clear(0);
        check_font(faces[i]);
    }
#else
    for (unsigned i = 0; i < sizeof(faces) / sizeof(faces[0]); ++i) {
        int top, rows;
        assert(font_glyph(faces[i], (uint8_t)extended[0], &top, &rows) == NULL);
    }
#endif
    puts("Classic Turkish glyph checks passed");
    return 0;
}
