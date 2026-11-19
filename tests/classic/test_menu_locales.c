// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui/gfx.h"
#include "ui/i18n.h"

int main(void)
{
    const hal_display_t panel = {.width = 128, .height = 64, .bpp = 1};
    uint8_t turkish = lang_find('t' << 8 | 'r');
    uint8_t russian = lang_find('r' << 8 | 'u');
    assert(turkish != LANG_ENGLISH && russian != LANG_ENGLISH && turkish != russian);
    assert(lang_built(turkish) && lang_built(russian));
    assert(lang_find(lang_value(turkish)) == turkish && lang_find(lang_value(russian)) == russian);
    assert(lang_set(turkish) && strcmp(tr("Settings"), "Ayarlar") == 0);
    assert(lang_set(russian));
    const char *title = tr("Settings");
    assert((uint8_t)title[0] == CYRILLIC_PREFIX);
    assert(gfx_init(&panel));
    assert(gfx_text_width(&font_5x8, title) == 9 * font_5x8.w);
    assert(gfx_text(&font_5x8, 0, 0, title, 1) == 9 * font_5x8.w);
    for (int i = 0; i < 9; i++) {
        bool ink = false;
        for (int y = 0; y < font_5x8.h; y++)
            for (int x = i * font_5x8.w; x < (i + 1) * font_5x8.w; x++)
                ink |= gfx_get_pixel(x, y) != 0;
        assert(ink);
    }
    char shortened[6];
    gfx_text_fit(&font_5x8, title, 3 * font_5x8.w, shortened, sizeof(shortened));
    assert(strlen(shortened) == 5 && shortened[4] == GLYPH_ELLIPSIS[0]);
    gfx_text_fit(&font_5x8, title, 100, shortened, sizeof(shortened));
    assert(strlen(shortened) == 4);
    assert(lang_set(LANG_ENGLISH) && strcmp(tr("Settings"), "Settings") == 0);
    puts("Classic Turkish and Russian menu switching and rendering passed");
    return 0;
}
