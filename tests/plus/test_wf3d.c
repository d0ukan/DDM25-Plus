/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus 3D waterfall (core/ui/wf3d.c, item 45): the drawing into a test frame buffer (gfx_fill / gfx_vline write
 * into it here), rows made with the waterfall's own ring (wfall.c). Built with -DCONFIG_SPECTRUM_WF=1
 * -DCONFIG_SPECTRUM_WF3D=1 by tools/test_air.py under ASan / UBSan. Checks: nothing without a buffer or rows; nothing
 * outside the box; a quiet front row is a line along the bottom in slot 1's colour, full width; a peak lifts the front
 * row by the amplitude at its point, in its level's slot colour; rows behind narrower towards the centre and higher; a
 * high front row hides the rows behind it (painter's order) and a flat one doesn't; only the newest WF3D_ROWS rows
 * (40 kept draw as their newest 24); the last row's highest peak reaches the top and no higher. */
#include <stdio.h>
#include <string.h>
#include "ui/wf3d.h"

static int failures;
#define CHECK(c, what) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

#define FW 240
#define FH 240
static color_t fb[FH][FW];
static long writes, outside;
static int bx0, bx1, by0, by1;	/* the box drawn into: [bx0, bx1) x [by0, by1) */

void gfx_fill(int x, int y, int w, int h, color_t c)
{
	for (int yy = y; yy < y + h; yy++)
		for (int xx = x; xx < x + w; xx++) {
			writes++;
			if (xx < bx0 || xx >= bx1 || yy < by0 || yy >= by1 || xx < 0 || xx >= FW || yy < 0 || yy >= FH) {
				outside++;
				continue;
			}
			fb[yy][xx] = c;
		}
}

void gfx_vline(int x, int y, int h, color_t c)
{
	gfx_fill(x, y, 1, h, c);
}

/* The screen's colours: background 100, slots 1..7 the values 1..7 (slot 0 is the background's) */
#define BG 100
static const color_t slot[WF_SLOTS] = { BG, 1, 2, 3, 4, 5, 6, 7 };

enum { X0 = 8, BW = 3, P = 64, TOP = 20, H = 150 };
static uint8_t buf[WF_BYTES];
static wf_t wf;

static void clear(void)
{
	for (int y = 0; y < FH; y++)
		for (int x = 0; x < FW; x++)
			fb[y][x] = BG;
	writes = outside = 0;
	bx0 = X0;
	bx1 = X0 + P * BW;
	by0 = TOP;
	by1 = TOP + H;
}

/* A sweep whose level is lv[i] at point i (a reading lv * WF_DB_LEVEL dB above a -120 dBm floor) */
static void push_levels(const uint8_t *lv)
{
	int16_t dbm[P];
	for (int i = 0; i < P; i++)
		dbm[i] = (int16_t)(-120 + lv[i] * WF_DB_LEVEL);
	wf_push(&wf, dbm, P, -120, wf.sweeps + 1);
}

static void push_flat(int n)
{
	uint8_t lv[P];
	memset(lv, 0, sizeof lv);
	for (int k = 0; k < n; k++)
		push_levels(lv);
}

static void draw(void)
{
	clear();
	wf3d_draw(&wf, X0, BW, TOP, H, slot, BG);
}

/* wf3d.c's geometry restated: row z's scale, the vanishing line, its base, a point's x */
static int sc(int z) { return 256 * WF3D_K / (WF3D_K + z); }
static int amp(void) { return H * WF3D_AMP_NUM / WF3D_AMP_DEN; }
static int yh(void) { int sb = sc((int)WF3D_ROWS - 1); return -((H - 1 - amp()) * sb) / (256 - sb); }
static int base_y(int z) { return TOP + yh() + (H - 1 - yh()) * sc(z) / 256; }
static int px(int i, int z) { int cx2 = 2 * X0 + P * BW; return (cx2 + (2 * (X0 + i * BW) + BW - cx2) * sc(z) / 256) / 2; }

/* The topmost pixel not background in column x (FH: none) */
static int top_drawn(int x)
{
	for (int y = 0; y < FH; y++)
		if (fb[y][x] != BG)
			return y;
	return FH;
}

int main(void)
{
	/* Nothing without a buffer or rows */
	wf_reset(&wf);
	draw();
	CHECK(writes == 0, "no buffer: nothing drawn");
	wf.buf = buf;
	wf_reset(&wf);
	draw();
	CHECK(writes == 0, "no rows: nothing drawn");

	/* One quiet row: a line along the bottom, slot 1's colour, from the first point to the last */
	push_flat(1);
	draw();
	int yb = TOP + H - 1;
	CHECK(base_y(0) == yb, "the front row's base at the box's bottom");
	CHECK(outside == 0, "inside the box");
	bool line = true;
	for (int x = px(0, 0); x <= px(P - 1, 0); x++)
		line = line && fb[yb][x] == slot[1];
	CHECK(line && fb[yb][px(0, 0) - 1] == BG && fb[yb][px(P - 1, 0) + 1] == BG, "a quiet front row: the bottom line");
	CHECK(px(0, 0) == X0 + BW / 2 && px(P - 1, 0) == X0 + (P - 1) * BW + BW / 2, "the front row as the bars' points");

	/* A peak at point 32 (level 15): lifted by the amplitude, in the strongest slot's colour; level 8 at point 10:
	 * about half as high, slot 4's colour */
	uint8_t lv[P];
	memset(lv, 0, sizeof lv);
	lv[32] = 15;
	lv[10] = 8;
	wf_reset(&wf);
	push_levels(lv);
	draw();
	CHECK(outside == 0, "inside the box");
	CHECK(top_drawn(px(32, 0)) == yb - amp() && fb[yb - amp()][px(32, 0)] == slot[wf_slot(15)], "the peak's height and colour");
	CHECK(top_drawn(px(10, 0)) == yb - amp() * 8 / 15 && fb[yb - amp() * 8 / 15][px(10, 0)] == slot[wf_slot(8)],
	      "a level 8 point: its height and colour");
	CHECK(fb[yb][px(32, 0)] == BG && fb[yb - 1][px(32, 0) + 1] == BG, "under the peak: cleared");

	/* 24 quiet rows: each a line, the ones behind higher and narrower (towards the centre) */
	wf_reset(&wf);
	push_flat((int)WF3D_ROWS);
	draw();
	CHECK(outside == 0, "inside the box");
	for (int z = 1; z < (int)WF3D_ROWS; z++) {
		int y = base_y(z);
		CHECK(y < base_y(z - 1) && px(0, z) >= px(0, z - 1) && px(P - 1, z) <= px(P - 1, z - 1), "perspective");
		CHECK(fb[y][px(0, z)] == slot[1] && fb[y][px(P - 1, z)] == slot[1], "row z's line, end to end");
		CHECK(fb[y][px(0, z) - 1] == BG || y == base_y(z - 1), "nothing left of row z's first point");
	}
	CHECK(px(0, (int)WF3D_ROWS - 1) - X0 > P * BW / 4, "the last row: under three fifths of the front's width");

	/* Painter's order: a high front row (every point level 15) hides the lines of the rows behind under it; a flat
	 * one hides nothing */
	int cx = px(P / 2, 1), y1 = base_y(1);
	CHECK(fb[y1][cx] == slot[1], "row 1's line seen behind a flat front row");
	memset(lv, 15, sizeof lv);
	push_levels(lv);	/* the newest: a wall */
	draw();
	CHECK(outside == 0, "inside the box");
	CHECK(fb[y1][cx] == BG, "row 1's line hidden behind the wall");
	CHECK(fb[yb - amp()][cx] == slot[wf_slot(15)], "the wall's ridge on top");
	int behind = base_y((int)WF3D_ROWS - 1);
	CHECK(behind < yb - amp() && fb[behind][px(P / 2, (int)WF3D_ROWS - 1)] == slot[1],
	      "the last row's line above the wall: still seen");

	/* Only the newest WF3D_ROWS rows: 40 kept draw as their newest 24 */
	static color_t once[FH][FW];
	wf_reset(&wf);
	for (int k = 0; k < (int)WF3D_ROWS; k++) {
		memset(lv, 0, sizeof lv);
		lv[k % P] = (uint8_t)(1 + k % 15);
		push_levels(lv);
	}
	draw();
	memcpy(once, fb, sizeof fb);
	wf_reset(&wf);
	for (int k = -16; k < (int)WF3D_ROWS; k++) {
		memset(lv, 0, sizeof lv);
		if (k < 0)
			memset(lv, 15, sizeof lv);	/* the 16 oldest: walls, never drawn */
		else
			lv[k % P] = (uint8_t)(1 + k % 15);
		push_levels(lv);
	}
	CHECK(wf.n == WF_ROWS, "40 rows kept");
	draw();
	CHECK(memcmp(once, fb, sizeof fb) == 0, "only the newest 24 drawn");

	/* Every row a wall: the last one's ridge reaches the top, nothing above it */
	wf_reset(&wf);
	memset(lv, 15, sizeof lv);
	for (int k = 0; k < (int)WF3D_ROWS; k++)
		push_levels(lv);
	draw();
	int highest = FH;
	for (int x = X0; x < X0 + P * BW; x++)
		if (top_drawn(x) < highest)
			highest = top_drawn(x);
	CHECK(outside == 0 && highest >= TOP && highest <= TOP + 2, "the last row's peaks at the top");

	/* A narrow box (bars 1 pixel wide, 32 points) and a short one: inside, no fault */
	wf_reset(&wf);
	int16_t dbm[32];
	for (int i = 0; i < 32; i++)
		dbm[i] = (int16_t)(i & 1 ? -60 : -120);
	for (int k = 0; k < 30; k++)
		wf_push(&wf, dbm, 32, -120, (uint32_t)k + 1);
	clear();
	bx1 = X0 + 32;
	by1 = TOP + 40;
	wf3d_draw(&wf, X0, 1, TOP, 40, slot, BG);
	CHECK(outside == 0 && writes > 0, "a narrow, short box");
	clear();
	wf3d_draw(&wf, X0, 1, TOP, 4, slot, BG);
	CHECK(writes == 0, "too short to draw: nothing");

	printf("3D waterfall drawing: %d failures\n", failures);
	return failures != 0;
}
