// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus art drawing (art.h). Only gfx_fill is used, one call a horizontal run of one color, so the clip, the
// 16-color palette of a bpp 4 panel and the changed-row marks all stay gfx's business.
#include "art.h"

#ifdef ART_HOST_TEST
void gfx_fill(int x, int y, int w, int h, uint16_t c);	// tests/plus/test_art.c: a test frame buffer
#else
#include "gfx.h"
#endif

#if ART_ENABLED

#define ART_GAP	6	// source pixels between the mark and the wordmark

void art_icon(unsigned id, int x, int y, uint16_t color)
{
	if (id >= ART_ICON_COUNT)
		return;
	const uint16_t *rows = art_icon_bits[id];
	for (int r = 0; r < ART_ICON_H; r++) {
		uint16_t bits = rows[r];
		int c = 0;
		while (bits != 0) {
			while (!(bits & 0x8000u)) {
				bits = (uint16_t)(bits << 1);
				c++;
			}
			int n = 0;
			while (bits & 0x8000u) {
				bits = (uint16_t)(bits << 1);
				n++;
			}
			gfx_fill(x + c, y + r, n, 1, color);
			c += n;
		}
	}
}

static unsigned mark_px(int r, int c)
{
	return ((unsigned)art_mark_bits[r * ART_MARK_STRIDE + (c >> 2)] >> (6 - 2 * (c & 3))) & 3u;
}

static int clamp_scale(int scale)
{
	return scale < 1 ? 1 : scale > 4 ? 4 : scale;
}

void art_mark(int x, int y, int scale, bool mono, uint16_t fg, uint16_t bg)
{
	scale = clamp_scale(scale);
	for (int r = 0; r < ART_MARK_H; r++) {
		int c = 0;
		while (c < ART_MARK_W) {
			unsigned v = mark_px(r, c);
			int n = 1;
			while (c + n < ART_MARK_W && mark_px(r, c + n) == v)
				n++;
			if (v != 0) {
				uint16_t col = mono ? (art_mark_ink[v - 1] ? fg : bg) : art_mark_rgb[v - 1];
				gfx_fill(x + c * scale, y + r * scale, n * scale, scale, col);
			}
			c += n;
		}
	}
}

static unsigned word_px(int r, int c)
{
	return ((unsigned)art_word_bits[r * ART_WORD_STRIDE + (c >> 3)] >> (7 - (c & 7))) & 1u;
}

void art_wordmark(int x, int y, int scale, uint16_t fg, uint16_t accent)
{
	scale = clamp_scale(scale);
	for (int r = 0; r < ART_WORD_H; r++) {
		int c = 0;
		while (c < ART_WORD_W) {
			if (!word_px(r, c)) {
				c++;
				continue;
			}
			int n = 1;	// a run stops at the split: its two sides differ in color
			while (c + n < ART_WORD_W && word_px(r, c + n) && c + n != ART_WORD_SPLIT)
				n++;
			gfx_fill(x + c * scale, y + r * scale, n * scale, scale, c >= ART_WORD_SPLIT ? accent : fg);
			c += n;
		}
	}
}

uint16_t art_accent(void)
{
	return art_mark_rgb[0];
}

int art_splash_h(int scale)
{
	scale = clamp_scale(scale);
	return (ART_MARK_H + ART_GAP + ART_WORD_H) * scale;
}

int art_splash_w(int scale)
{
	scale = clamp_scale(scale);
	return (ART_MARK_W > ART_WORD_W ? ART_MARK_W : ART_WORD_W) * scale;
}

int art_splash(int cx, int y, int scale, bool mono, uint16_t fg, uint16_t bg)
{
	scale = clamp_scale(scale);
	art_mark(cx - ART_MARK_W * scale / 2, y, scale, mono, fg, bg);
	art_wordmark(cx - ART_WORD_W * scale / 2, y + (ART_MARK_H + ART_GAP) * scale, scale, fg,
		     mono ? fg : art_accent());
	return art_splash_h(scale);
}

#endif
