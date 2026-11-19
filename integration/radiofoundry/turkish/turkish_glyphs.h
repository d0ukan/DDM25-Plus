// SPDX-License-Identifier: MIT
// Private to fonts.c. The glyphs extend the one-byte UI text encoding.
#include "fonts/turkish_jbmono.inc"

// Glyphs drawn for the Classic 5x8 cell, left-aligned in each byte.
static const uint8_t turkish_5x8[][8] = {
    {0x50, 0x20, 0x70, 0x80, 0xb0, 0x90, 0x70, 0x00}, // Ğ
    {0x50, 0x20, 0x00, 0x70, 0x90, 0x70, 0x10, 0x60}, // ğ
    {0x20, 0x00, 0x70, 0x20, 0x20, 0x20, 0x70, 0x00}, // İ
    {0x00, 0x00, 0x00, 0x60, 0x20, 0x20, 0x70, 0x00}, // ı
    {0x70, 0x80, 0x60, 0x10, 0xe0, 0x20, 0x40, 0x00}, // Ş
    {0x00, 0x00, 0x70, 0xc0, 0x30, 0xe0, 0x20, 0x40}, // ş
    {0x70, 0x80, 0x80, 0x80, 0x70, 0x20, 0x40, 0x00}, // Ç
    {0x00, 0x00, 0x70, 0x80, 0x80, 0x70, 0x20, 0x40}, // ç
    {0x50, 0x00, 0x60, 0x90, 0x90, 0x90, 0x60, 0x00}, // Ö
    {0x00, 0x50, 0x00, 0x60, 0x90, 0x90, 0x60, 0x00}, // ö
    {0x50, 0x00, 0x90, 0x90, 0x90, 0x90, 0x60, 0x00}, // Ü
    {0x00, 0x50, 0x00, 0x90, 0x90, 0x90, 0x70, 0x00}, // ü
};

static int turkish_slot(uint8_t c)
{
    if (c >= GLYPH_TURKISH_FIRST && c <= GLYPH_TURKISH_LAST)
        return c - GLYPH_TURKISH_FIRST;
    // Turkish letters that use their existing Latin-1 byte values.
    static const uint8_t latin1[] = {0xc7, 0xe7, 0xd6, 0xf6, 0xdc, 0xfc};
    for (unsigned i = 0; i < sizeof(latin1); ++i)
        if (c == latin1[i])
            return (int)(i + GLYPH_TURKISH_COUNT);
    return -1;
}

static const uint8_t *turkish_glyph(const font_t *f, unsigned slot, int *top, int *rows)
{
    if (f == &font_5x8) {
        *top = 0;
        *rows = 8;
        return turkish_5x8[slot];
    }

    static const struct {
        const font_t *font;
        const uint8_t *bits;
        const uint16_t *index;
    } faces[] = {
        {&font_aa_6x12, turkish_6x12_bits, turkish_6x12_index},
        {&font_aa_8x16, turkish_8x16_bits, turkish_8x16_index},
#if CONFIG_FONT_BIG
        {&font_aa_9x18, turkish_9x18_bits, turkish_9x18_index},
#endif
        {&font_aa_12x24, turkish_12x24_bits, turkish_12x24_index},
#if CONFIG_FONT_XL
        {FONT_AA_LARGEST, turkish_16x32_bits, turkish_16x32_index},
#endif
    };
    for (unsigned i = 0; i < sizeof(faces) / sizeof(faces[0]); ++i) {
        if (faces[i].font == f) {
            const uint8_t *glyph = faces[i].bits + faces[i].index[slot];
            *top = glyph[0];
            *rows = glyph[1];
            return glyph + 2;
        }
    }
    return NULL;
}
