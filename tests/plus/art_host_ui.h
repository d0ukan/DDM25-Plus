// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-ins for the firmware's gfx, layout, colors and sbuf that splash_plus.c uses, so tests/plus/test_art.c can
// draw the whole DDM25 Plus boot screen on the host (included by splash_plus.c under ART_HOST_TEST). The signatures
// follow core/ui/gfx.h, core/ui/ui_screens.h and core/fmt.h; the bodies are in test_art.c.
#ifndef ART_HOST_UI_H
#define ART_HOST_UI_H

#include <stddef.h>
#include <stdint.h>

typedef uint16_t color_t;
typedef struct {
	int w, h;	// a fixed cell: every glyph w x h
} font_t;
typedef struct {
	const font_t *tiny, *small, *medium;
} ui_layout_t;
typedef struct {
	color_t bg, fg, dim;
} ui_colors_t;
typedef struct {
	char *p;
	char *end;
} sbuf_t;

extern ui_layout_t ui_layout;
extern ui_colors_t ui_col;

int gfx_width(void);
int gfx_height(void);
int gfx_bpp(void);
void gfx_clear(color_t c);
void gfx_hline(int x, int y, int w, color_t c);
int gfx_text(const font_t *f, int x, int y, const char *s, color_t fg);
int gfx_text_width(const font_t *f, const char *s);
void sb_init(sbuf_t *b, char *buf, size_t len);
void sb_str(sbuf_t *b, const char *s);

#endif
