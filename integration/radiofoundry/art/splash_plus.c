// SPDX-License-Identifier: GPL-3.0-or-later
// The DM32 Plus boot screen (CONFIG_PLUS_ART): the mark and the wordmark (art.c), a rule, the owner's two boot text
// lines (the CPS boot text, left out when empty) and board and version small and dim along the bottom. Called first
// thing in ui_draw_splash (tools/plus_features.py), so the boot path and the stub entry screen both get it.
// tools/art_build.py splash_layout() mirrors this layout for the mock-up; keep the two in step.
#include "art.h"
#include "gfx.h"
#include "ui_screens.h"
#include "fmt.h"

#if ART_ENABLED

bool ui_draw_splash_plus(const char *board_name, const char *version, const char *line1, const char *line2)
{
	int w = gfx_width(), h = gfx_height();
	const ui_layout_t *L = &ui_layout;
	// 2x on panels with room for it (240 x 320: a 112 px mark), else 1x; too small even for that: the stock screen
	int k = w >= art_splash_w(2) + 16 && h >= 200 ? 2 : 1;
	if (w < art_splash_w(1) + 4 || h < art_splash_h(1) + L->tiny->h + 4)
		return false;
	bool mono = gfx_bpp() == 1;
	gfx_clear(ui_col.bg);

	const char *lines[2] = { line1 != NULL ? line1 : "", line2 != NULL ? line2 : "" };
	int n_lines = (lines[0][0] != '\0') + (lines[1][0] != '\0');
	const font_t *lf = L->medium;
	for (int i = 0; i < 2; i++)
		if (gfx_text_width(lf, lines[i]) > w - 2)
			lf = L->small;

	// Footer as the stock screen: board and version, one space between when two don't fit, then the version alone
	const font_t *ff = L->tiny;
	char foot[48];
	static const char *const seps[] = { "  ", " ", NULL };
	for (int i = 0; i < 3; i++) {
		sbuf_t b;
		sb_init(&b, foot, sizeof foot);
		if (seps[i] != NULL) {
			sb_str(&b, board_name);
			sb_str(&b, seps[i]);
		}
		sb_str(&b, version);
		if (gfx_text_width(ff, foot) <= w)
			break;
	}
	int foot_y = h - ff->h;

	// Stack: art, rule, lines; centered above the footer. Lines that don't fit under the art are left out (the
	// stock screen never drops them, but it shrinks its mark; the art has two sizes only)
	int gap = 6 * k;
	int art_h = art_splash_h(k);
	int total = art_h + gap + 1;
	while (n_lines > 0 && total + gap + n_lines * lf->h > foot_y - 2) {
		lines[lines[1][0] != '\0' ? 1 : 0] = "";	// the last one shown goes first
		n_lines--;
	}
	if (n_lines > 0)
		total += gap + n_lines * lf->h;
	int y = (foot_y - total) / 2;
	if (y < 0)
		y = 0;
	y += art_splash(w / 2, y, k, mono, ui_col.fg, ui_col.bg);
	y += gap;
	gfx_hline(w / 2 - w / 6, y, w / 3, mono ? ui_col.fg : art_accent());
	y += 1;
	if (n_lines > 0) {
		y += gap;
		for (int i = 0; i < 2; i++) {
			if (lines[i][0] == '\0')
				continue;
			gfx_text(lf, (w - gfx_text_width(lf, lines[i])) / 2, y, lines[i], ui_col.fg);
			y += lf->h;
		}
	}
	gfx_text(ff, (w - gfx_text_width(ff, foot)) / 2, foot_y, foot, ui_col.dim);
	return true;
}

#endif
