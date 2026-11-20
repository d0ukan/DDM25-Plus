/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus 3D waterfall (wf3d.h): the drawing. Installed as core/ui/wf3d.c. Integers only; nothing kept. */
#include "config.h"
#if CONFIG_SPECTRUM_WF3D
#include "ui/wf3d.h"

/* Row z's scale in 1/256: WF3D_K / (WF3D_K + z) */
static int scale(int z)
{
	return 256 * WF3D_K / (WF3D_K + z);
}

/* One column of a ridge: the area under it down to base cleared, then the ridge from py (the column before, py < 0:
 * none) to y */
static void column(int x, int y, int py, int base, color_t c, color_t bg)
{
	int y0 = py < 0 || py > y ? y : py, y1 = py > y ? py : y;
	if (base > y1)
		gfx_vline(x, y1 + 1, base - y1, bg);
	gfx_vline(x, y0, y1 - y0 + 1, c);
}

void wf3d_draw(const wf_t *w, int x0, int bw, int top, int h, const color_t slot[WF_SLOTS], color_t bg)
{
	int n = w->buf != NULL ? (w->n < WF3D_ROWS ? w->n : (int)WF3D_ROWS) : 0, p = w->points;
	if (n == 0 || p < 2 || bw < 1 || h < 8)
		return;
	/* The front row's base at the box's bottom (H below its top), its peaks up to A; the vanishing line yh (above the
	 * box: negative) such that the last row's highest peak lands on the top. Every y here is relative to top */
	int H = h - 1, A = h * WF3D_AMP_NUM / WF3D_AMP_DEN, sb = scale((int)WF3D_ROWS - 1);
	int yh = -((H - A) * sb) / (256 - sb);
	int cx2 = 2 * x0 + p * bw;	/* twice the centre */
	for (int z = n - 1; z >= 0; z--) {	/* back to front */
		int s = scale(z);
		int base = yh + (H - yh) * s / 256;
		int py = -1, px = 0;
		for (int i = 0; i < p; i++) {
			uint8_t l = wf_get(w, (uint8_t)z, (uint8_t)i);
			int x = (cx2 + (2 * (x0 + i * bw) + bw - cx2) * s / 256) / 2;
			int y = base - A * l * s / (15 * 256);
			if (y < 0)
				y = 0;
			/* the segment from the point before: its colour by the higher of its two levels */
			uint8_t hi = l;
			if (i > 0) {
				uint8_t lp = wf_get(w, (uint8_t)z, (uint8_t)(i - 1));
				hi = lp > l ? lp : l;
			}
			uint8_t sl = wf_slot(hi);
			color_t c = slot[sl != 0 ? sl : 1u];
			if (i == 0 || x == px) {
				column(x, y + top, i == 0 ? -1 : py + top, base + top, c, bg);
			} else {
				int ya = py;
				for (int xx = px + 1; xx <= x; xx++) {
					int yy = ya + (y - ya) * (xx - px) / (x - px);
					column(xx, yy + top, py + top, base + top, c, bg);
					py = yy;
				}
			}
			px = x;
			py = y;
		}
	}
}
#endif
