/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 16, the spectrum in colour (CONFIG_SPECTRUM_COLOR, docs/plus.md "Round 16: airfields by province,
 * spectrum in colour"): Tools > Spectrum's waterfall and 3D view through the real UI in upstream's simulator
 * (tests/plus/ui_sim_toolbox.c: the real sweep engine; gfx at bpp 4 with its 16 entry palette, as the DM-32UV's
 * panel), in the dark and the light theme, in English and in Turkish. Built by tools/test_airprov.py like
 * tools/test_air.py builds the 3D view's flows, with the switch. Each case runs in a child process. Checks: the newest
 * waterfall row draws every point in its level's ramp colour (wf_ramp: a level's slot), exactly, so no colour fell to
 * the palette's nearest; the 3D view draws every slot of the sweep's levels in the ramp; the colours on the whole
 * screen counted (16 at most, printed); every view fits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "hal_time.h"
#include "ui/gfx.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/ui_screens.h"
#include "ui/i18n.h"
#include "ui/rpt.h"
#include "ui/wfall.h"
#include "ui_sim_toolbox.h"

/* Tools > Repeaters' table (the map draws it; not used here) */
#define P(la, lo) (int32_t)((la) * 1e5 + 0.5), (int32_t)((lo) * 1e5 + 0.5)
const rpt_entry_t rpt_table[] = {
	{ P(39.93, 32.86), 145600000, -480, 885, RPT_FM, 0, "TA2RAK", "Ankara", 6 },
};
const rpt_info_t rpt_info = { sizeof rpt_table / sizeof rpt_table[0], 0, "2026-11-21", "test" };

static int fails;
static const char *name;
static bool turkish, light;

#define T(en, tr) (turkish ? (tr) : (en))

static void ok_run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s (%s %s): %s: %s\n", name, turkish ? "tr" : "en", light ? "light" : "dark", script, err);
		fails++;
	}
}

static void expect(const char *e)
{
	char buf[500], *o = buf;
	while (*e && o < buf + sizeof buf - 16) {
		while (*e == ' ')
			e++;
		if (!*e)
			break;
		memcpy(o, "expect:", 7);
		o += 7;
		while (*e && *e != ' ' && o < buf + sizeof buf - 2)
			*o++ = *e++;
		*o++ = ' ';
	}
	*o = 0;
	ok_run(buf);
}

static void check(bool c, const char *what)
{
	if (!c) {
		printf("FAIL %s (%s %s): %s\n", name, turkish ? "tr" : "en", light ? "light" : "dark", what);
		fails++;
	}
}

static void layout(void)
{
	char issues[SIM_LAYOUT_MAX][160];
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int k = 0; k < n; k++)
		printf("FAIL %s (%s): layout: %s\n", name, turkish ? "tr" : "en", issues[k]);
	fails += n != 0;
}

static void start(const char *n)
{
	name = n;
	tb_sim_clear();
	tb_sim.gps = true;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);
	const char *shots = getenv("AIRPROV_SHOTS");
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? (light ? "wfc-tr-light-" : "wfc-tr-") : (light ? "wfc-en-light-" : "wfc-en-"));
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	check(gfx_bpp() == 4, "the DM-32's 16 colour panel");
	if (light)
		ui_theme_set(1);
}

/* The sweep: the last 15 points at levels 1..15 above a -120 dBm floor, as the engine reads them */
static uint32_t fclk;
static void sweeps(int n)
{
	for (int s = 0; s < n; s++) {
		tb_sim_changed();
		ok_run("wait:100");
		if (fclk < hal_millis())
			fclk = hal_millis();
		spectrum_view_t v;
		spectrum_snapshot(&v);
		for (uint8_t i = 0; i < v.points && spectrum_on(); i++) {
			fclk += 40;
			int k = v.points - 1 - i;	/* the last 15 points levels 1..15, the others the floor (the median) */
			spectrum_service((int16_t)(-120 + (k < 15 ? 15 - k : 0) * WF_DB_LEVEL), fclk);
		}
	}
	tb_sim_changed();
	ok_run("wait:300");
}

/* Distinct colours on the screen */
static int colours(void)
{
	color_t seen[64];
	int n = 0;
	for (int y = 0; y < gfx_height(); y++)
		for (int x = 0; x < gfx_width(); x++) {
			color_t c = gfx_get_pixel(x, y);
			int k = 0;
			while (k < n && seen[k] != c)
				k++;
			if (k == n && n < 64)
				seen[n++] = c;
		}
	return n;
}

static int pixels(color_t c)
{
	int n = 0;
	for (int y = 0; y < gfx_height(); y++)
		for (int x = 0; x < gfx_width(); x++)
			n += gfx_get_pixel(x, y) == c;
	return n;
}

static void views(void)
{
	start("WFC flat and 3D in colour");
	color_t slot[WF_SLOTS];
	wf_ramp(slot, ui_col.bg, ui_col.rx, ui_col.tx);
	ui_push(SCR_SPECTRUM, 0);
	sweeps(3);
	spectrum_view_t v;
	spectrum_snapshot(&v);
	check(v.points >= 16, "a sweep");
	int w = gfx_width(), bw = v.points ? w / v.points : 1, x0 = (w - bw * v.points) / 2;
	/* The newest row: the top of the strongest point's column in the waterfall (red: nothing else is) */
	int xs = x0 + (v.points - 1) * bw + bw / 2, y0 = -1;
	for (int y = 0; y < gfx_height() && y0 < 0; y++)
		if (gfx_get_pixel(xs, y) == slot[7])
			y0 = y;
	check(y0 > 0, "the strongest point's row in red");
	if (getenv("WFC_DEBUG") != NULL) {
		printf("palette:");
		for (int k = 0; k < 16; k++)
			printf(" %04x", gfx_palette((uint8_t)k));
		printf("\nslots:");
		for (int k = 0; k < 8; k++)
			printf(" %04x", slot[k]);
		printf("\nlevels:");
		for (uint8_t i = 0; i < v.points; i++)
			printf(" %u", wf_level(v.dbm[i], v.floor_dbm));
		printf(" floor %d\ncolumn:", v.floor_dbm);
		for (int y = 0; y < gfx_height(); y += 4)
			printf(" %04x", gfx_get_pixel(xs, y));
		printf("\n");
	}
	int wrong = 0;
	bool seen[WF_SLOTS] = { false };
	for (uint8_t i = 0; y0 > 0 && i < v.points; i++) {
		uint8_t s = wf_slot(wf_level(v.dbm[i], v.floor_dbm));
		seen[s] = true;
		if (i == v.points / 2u)
			continue;	/* the centre's mark over the newest row */
		if (gfx_get_pixel(x0 + i * bw + bw / 2, y0) != slot[s]) {
			if (wrong++ < 4)
				printf("  point %u level %u: 0x%04x, want slot %u 0x%04x\n", i, wf_level(v.dbm[i], v.floor_dbm),
				       gfx_get_pixel(x0 + i * bw + bw / 2, y0), s, slot[s]);
		}
	}
	check(wrong == 0, "the waterfall: each point in its level's ramp colour");
	check(seen[1] && seen[4] && seen[7], "weak, middle and strong levels in the sweep");
	int n_flat = colours();
	check(n_flat <= 16, "the flat view: 16 colours at most");
	expect(T("shows=Floor_-120dBm", "shows=Taban_-120dBm"));
	layout();
	ok_run("shot:waterfall-colour");
	/* *: 3D, each slot of the sweep's levels drawn in its ramp colour */
	ok_run("quick wait:100");
	expect(T("screen=spectrum toast=3D_waterfall", "screen=spectrum toast=3B_"));
	ok_run("wait:2000");
	sweeps(1);
	for (uint8_t s = 1; s < WF_SLOTS; s++)
		if (seen[s]) {
			char what[64];
			snprintf(what, sizeof what, "3D: slot %u's ramp colour drawn", s);
			check(pixels(slot[s]) > 0, what);
		}
	int n_3d = colours();
	check(n_3d <= 16, "the 3D view: 16 colours at most");
	layout();
	ok_run("shot:waterfall-3d-colour");
	if (!turkish)
		printf("spectrum in colour (%s theme): %d colours on the screen with the waterfall, %d with the 3D view\n",
		       light ? "light" : "dark", n_flat, n_3d);
	/* the bars alone: no ramp colour but the theme's own */
	ok_run("quick wait:100 wait:2000");
	tb_sim_changed();
	ok_run("wait:100");
	check(pixels(slot[1]) == 0 && pixels(slot[2]) == 0 && pixels(slot[3]) == 0, "bars alone: no waterfall colours");
	ok_run("back wait:100");
}

typedef void (*case_fn)(void);
static const case_fn all[] = { views };

int main(void)
{
	int bad = 0, cases = 0;
	for (int l = 0; l < 4; l++) {
		for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				turkish = l & 1;
				light = l >= 2;
				all[i]();
				fflush(stdout);
				_exit(fails != 0);
			}
			int st = 0;
			waitpid(pid, &st, 0);
			cases++;
			if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
				bad++;
				if (!WIFEXITED(st))
					printf("FAIL case %u (%d): crashed\n", i, l);
			}
		}
	}
	printf("Plus spectrum in colour UI: %d cases (English and Turkish, dark and light), %d failed\n", cases, bad);
	return bad != 0;
}
