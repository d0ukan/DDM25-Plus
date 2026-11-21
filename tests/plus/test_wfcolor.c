/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 16, the spectrum in colour (CONFIG_SPECTRUM_COLOR, wfall.h wf_ramp): the ramp for the dark, light
 * and high contrast themes, and the 3D view drawn with it into a test frame buffer (gfx_fill / gfx_vline write into it
 * here; core/ui/wf3d.c and wfall.c real). Built with -DCONFIG_SPECTRUM_WF=1 -DCONFIG_SPECTRUM_WF3D=1
 * -DCONFIG_SPECTRUM_COLOR=1 by tools/test_airprov.py under ASan / UBSan. Checks: slot 0 the background, slot 4 the
 * theme's RX, slot 7 its TX; the eight colours distinct and none the background, at most five that the theme does not
 * have; the hue running blue > cyan > green > yellow > orange > red; on white the weakest light and every colour darker
 * than white enough to see; each level 1..15 of the front row's ridge drawn in its slot's ramp colour. */
#include <stdio.h>
#include <string.h>
#include "ui/wf3d.h"

static int failures;
#define CHECK(c, what) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

#define FW 240
#define FH 200
static color_t fb[FH][FW];

void gfx_fill(int x, int y, int w, int h, color_t c)
{
	for (int yy = y; yy < y + h; yy++)
		for (int xx = x; xx < x + w; xx++)
			if (xx >= 0 && xx < FW && yy >= 0 && yy < FH)
				fb[yy][xx] = c;
}

void gfx_vline(int x, int y, int h, color_t c)
{
	gfx_fill(x, y, 1, h, c);
}

static int r5(color_t c) { return c >> 11; }
static int g6(color_t c) { return (c >> 5) & 0x3F; }
static int b5(color_t c) { return c & 0x1F; }
/* 0..255 per channel */
static int R(color_t c) { return r5(c) * 255 / 31; }
static int G(color_t c) { return g6(c) * 255 / 63; }
static int B(color_t c) { return b5(c) * 255 / 31; }
static int luma(color_t c) { return (R(c) * 299 + G(c) * 587 + B(c) * 114) / 1000; }

/* The hue in degrees 0..359 (-1: grey) */
static int hue(color_t c)
{
	int r = R(c), g = G(c), b = B(c), mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
	int mn = r < g ? (r < b ? r : b) : (g < b ? g : b), d = mx - mn;
	if (d < 24)
		return -1;
	int h = mx == r ? 60 * (g - b) / d : mx == g ? 120 + 60 * (b - r) / d : 240 + 60 * (r - g) / d;
	return (h + 360) % 360;
}

typedef struct {
	const char *name;
	color_t bg, rx, tx, theme[8];	/* the theme's colours on the spectrum screen */
} theme_t;

/* ui_screens.c's themes (dark, light, high contrast) */
static const theme_t themes[] = {
	{ "dark", RGB565(0, 0, 0), RGB565(40, 184, 72), RGB565(224, 32, 32),
	  { RGB565(0, 0, 0), RGB565(255, 255, 255), RGB565(168, 168, 168), RGB565(72, 72, 72), RGB565(224, 32, 32),
	    RGB565(40, 184, 72), RGB565(240, 168, 0), RGB565(96, 96, 96) } },
	{ "light", RGB565(255, 255, 255), RGB565(0, 136, 40), RGB565(200, 0, 0),
	  { RGB565(255, 255, 255), RGB565(0, 0, 0), RGB565(88, 88, 88), RGB565(176, 176, 176), RGB565(200, 0, 0),
	    RGB565(0, 136, 40), RGB565(176, 96, 0), RGB565(128, 128, 128) } },
	{ "high contrast", RGB565(0, 0, 0), RGB565(0, 255, 0), RGB565(255, 0, 0),
	  { RGB565(0, 0, 0), RGB565(255, 255, 255), RGB565(255, 0, 0), RGB565(0, 255, 0), RGB565(255, 255, 0) } },
};

static void ramp_checks(const theme_t *t)
{
	color_t s[WF_SLOTS];
	char what[96];
	wf_ramp(s, t->bg, t->rx, t->tx);
	snprintf(what, sizeof what, "%s: background, RX and TX reused", t->name);
	CHECK(s[0] == t->bg && s[4] == t->rx && s[7] == t->tx, what);
	int added = 0;
	for (unsigned a = 1; a < WF_SLOTS; a++) {
		snprintf(what, sizeof what, "%s: slot %u distinct and not the background", t->name, a);
		for (unsigned b = 0; b < a; b++)
			CHECK(s[a] != s[b], what);
		bool known = false;
		for (unsigned k = 0; k < 8; k++)
			known = known || (t->theme[k] != 0 && t->theme[k] == s[a]) || s[a] == t->bg;
		added += !known;
	}
	snprintf(what, sizeof what, "%s: at most five palette entries added (%d)", t->name, added);
	CHECK(added <= 5, what);
	/* blue (dark blue, blue, cyan: 180..250), green (90..150), yellow (50..65), orange (20..40), red (<15 or >345) */
	int h[WF_SLOTS];
	for (unsigned a = 1; a < WF_SLOTS; a++)
		h[a] = hue(s[a]);
	snprintf(what, sizeof what, "%s: the hues run blue, cyan, green, yellow, orange, red (%d %d %d %d %d %d %d)", t->name,
		 h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
	CHECK(h[1] >= 200 && h[1] <= 250 && h[2] >= 200 && h[2] <= 240 && h[3] >= 175 && h[3] < 200, what);
	CHECK(h[4] >= 90 && h[4] <= 150 && h[5] >= 48 && h[5] <= 65 && h[6] >= 20 && h[6] <= 40 &&
	      (h[7] <= 15 || h[7] >= 345), what);
	bool light = luma(t->bg) > 128;
	if (light) {
		snprintf(what, sizeof what, "%s: the weakest light, the rest readable on white", t->name);
		CHECK(luma(s[1]) > 170, what);
		for (unsigned a = 2; a < WF_SLOTS; a++)
			CHECK(luma(s[a]) < 175, what);
	} else {
		snprintf(what, sizeof what, "%s: the weakest dark, every slot visible on black", t->name);
		CHECK(luma(s[1]) < 80 && luma(s[1]) > 25, what);
		for (unsigned a = 2; a < WF_SLOTS; a++)
			CHECK(luma(s[a]) > 60, what);
	}
}

/* The 3D view with the ramp: a front row whose point i has level levels[i]; each level's ridge top in its colour */
enum { X0 = 8, BW = 3, P = 64, TOP = 10, H = 180 };
static uint8_t buf[WF_BYTES];

static void ridge_checks(const theme_t *t)
{
	color_t s[WF_SLOTS];
	wf_ramp(s, t->bg, t->rx, t->tx);
	wf_t w;
	w.buf = buf;
	wf_reset(&w);
	/* runs of four points per level 0..15: the segment into a run's third point has both ends at its level */
	int16_t dbm[P];
	for (int i = 0; i < P; i++)
		dbm[i] = (int16_t)(-120 + (i / 4) * WF_DB_LEVEL);
	wf_push(&w, dbm, P, -120, 1);
	for (int y = 0; y < FH; y++)
		for (int x = 0; x < FW; x++)
			fb[y][x] = t->bg;
	wf3d_draw(&w, X0, BW, TOP, H, s, t->bg);
	int amp = H * WF3D_AMP_NUM / WF3D_AMP_DEN, yb = TOP + H - 1;
	char what[96];
	for (int l = 1; l < 16; l++) {
		int i = l * 4 + 2, x = X0 + i * BW + BW / 2, y = yb - amp * l / 15;
		snprintf(what, sizeof what, "%s: level %d's ridge in slot %u's colour", t->name, l, wf_slot((uint8_t)l));
		CHECK(fb[y][x] == s[wf_slot((uint8_t)l)], what);
	}
	snprintf(what, sizeof what, "%s: a quiet run along the bottom in slot 1's colour", t->name);
	CHECK(fb[yb][X0 + 2 * BW + BW / 2] == s[1], what);
}

int main(void)
{
	for (unsigned k = 0; k < sizeof themes / sizeof themes[0]; k++) {
		ramp_checks(&themes[k]);
		ridge_checks(&themes[k]);
	}
	printf("spectrum colour ramp: %u themes, %d failures\n", (unsigned)(sizeof themes / sizeof themes[0]), failures);
	return failures != 0;
}
