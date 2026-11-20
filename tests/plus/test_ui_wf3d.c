/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus 3D waterfall (item 45, docs/plus.md "Round 14: air band and spectrum"): Tools > Spectrum's views through
 * the real UI in upstream's simulator (tests/plus/ui_sim_toolbox.c: the real sweep engine, the heap the waterfall
 * takes, counted), on the DM-32UV's display class, in English and in Turkish. * cycles waterfall > 3D > bars alone >
 * waterfall: the 3D view keeps the waterfall's buffer and rows (no second block), draws the strong point's ridge in the
 * strongest colour, and the bars alone give the buffer back; without heap room the 3D view says so as the waterfall
 * does; every view fits. Each case runs in a child process. */
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
#include "ui/i18n.h"
#include "ui/rpt.h"
#include "ui/wfall.h"
#include "ui_sim_toolbox.h"

/* Tools > Repeaters' table (the map draws it; not used here) */
#define P(la, lo) (int32_t)((la) * 1e5 + 0.5), (int32_t)((lo) * 1e5 + 0.5)
const rpt_entry_t rpt_table[] = {
	{ P(39.93, 32.86), 145600000, -480, 885, RPT_FM, 0, "TA2RAK", "Ankara", 6 },
};
const rpt_info_t rpt_info = { sizeof rpt_table / sizeof rpt_table[0], 0, "2026-11-20", "test" };

static int fails;
static const char *name;
static bool turkish;

#define T(en, tr) (turkish ? (tr) : (en))

static void ok_run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s (%s): %s: %s\n", name, turkish ? "tr" : "en", script, err);
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
		printf("FAIL %s (%s): %s\n", name, turkish ? "tr" : "en", what);
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
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_M);	/* the DM-32 has Left / Right, * and # (map M) */
	const char *shots = getenv("AIR_SHOTS");
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "wf3d-tr-" : "wf3d-en-");
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
}

/* The spectrum engine: n sweeps with a strong signal at point k (the floor at -120 dBm), as test_ui_toolbox.c */
static uint32_t fclk;
static void sweeps(int n, uint8_t k)
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
			spectrum_service(i == k ? -60 : -120, fclk);
		}
	}
	tb_sim_changed();
	ok_run("wait:300");
}

/* Pixels in a colour within rows y0..y1 */
static int pixels(color_t c, int y0, int y1)
{
	int n = 0;
	for (int y = y0; y <= y1 && y < gfx_height(); y++)
		for (int x = 0; x < gfx_width(); x++)
			n += gfx_get_pixel(x, y) == c;
	return n;
}

/* The highest row holding a pixel in colour c (gfx_height(): none) */
static int highest(color_t c)
{
	for (int y = 0; y < gfx_height(); y++)
		for (int x = 0; x < gfx_width(); x++)
			if (gfx_get_pixel(x, y) == c)
				return y;
	return gfx_height();
}

static void views(void)
{
	start("WF3D views");
	ui_push(SCR_SPECTRUM, 0);
	check(ui_spectrum_wf_held() && tb_sim.heap_allocs == 1, "the waterfall's buffer taken");
	sweeps(4, 10);
	int flat_tx = pixels(ui_col.tx, 0, gfx_height() - 1);
	check(flat_tx > 0, "the waterfall: the strong point's rows");
	int flat_top = highest(ui_col.tx);
	ok_run("shot:waterfall");
	/* *: 3D, the same buffer and rows */
	ok_run("quick wait:100");
	expect(T("screen=spectrum toast=3D_waterfall", "screen=spectrum toast=3B_"));
	check(ui_spectrum_wf_held() && tb_sim.heap_allocs == 1 && tb_sim.heap_frees == 0, "3D: the same block");
	ok_run("wait:2000");
	tb_sim_changed();
	ok_run("wait:100");
	int ridge = pixels(ui_col.tx, 0, gfx_height() - 1);
	check(ridge > 0, "3D: the strong point's ridges in the strongest colour");
	check(highest(ui_col.tx) > flat_top, "3D: the newest in front, at the bottom (the waterfall's newest is on top)");
	expect(T("shows=Floor_-120dBm", "shows=Taban_-120dBm"));
	layout();
	ok_run("shot:waterfall-3d");
	/* sweeps go on in 3D: drawn from the same rows */
	sweeps(2, 20);
	check(pixels(ui_col.tx, 0, gfx_height() - 1) > 0 && ui_spectrum_wf_held(), "3D: new sweeps drawn");
	/* *: bars alone, the buffer back */
	ok_run("quick wait:100");
	expect(T("toast=Waterfall_off", "toast="));
	check(!ui_spectrum_wf_held() && tb_sim.heap_frees == 1 && tb_sim.heap_held == 0, "bars alone: given back");
	ok_run("wait:2000");
	tb_sim_changed();
	ok_run("wait:100");
	check(pixels(ui_col.tx, 0, gfx_height() - 1) == 0, "bars alone: no rows drawn");
	layout();
	/* *: the waterfall again, taken again */
	ok_run("quick wait:100");
	expect(T("toast=Waterfall_on", "toast="));
	check(ui_spectrum_wf_held() && tb_sim.heap_allocs == 2, "the waterfall: taken again");
	/* Back gives it back; the view chosen stays for the next entry (RAM only) */
	ok_run("quick wait:100 back wait:100");
	check(!ui_spectrum_wf_held() && tb_sim.heap_allocs == tb_sim.heap_frees, "Back: given back");
	ui_push(SCR_SPECTRUM, 0);
	check(ui_spectrum_wf_held(), "3D kept for the next entry: taken");
	sweeps(2, 10);
	check(pixels(ui_col.tx, 0, gfx_height() - 1) > 0, "3D again");
	ok_run("back wait:100");
	check(tb_sim.heap_allocs == tb_sim.heap_frees, "nothing leaks");
}

static void no_room(void)
{
	start("WF3D without heap");
	tb_sim.heap_fail = true;
	ui_push(SCR_SPECTRUM, 0);
	sweeps(2, 10);
	expect(T("shows=No_room", "shows=Yer_yok"));
	ok_run("quick wait:100");
	expect(T("toast=Waterfall:_no_room", "toast="));
	check(!ui_spectrum_wf_held() && tb_sim.heap_refused >= 1, "refused, bars alone");
	expect(T("shows=No_room shows=Floor_-120dBm", "shows=Yer_yok"));
	layout();
	ok_run("quick wait:100");
	expect(T("toast=Waterfall_off hides=No_room", "hides=Yer_yok"));
}

typedef void (*case_fn)(void);
static const case_fn all[] = { views, no_room };

int main(void)
{
	int bad = 0, cases = 0;
	for (int l = 0; l < 2; l++) {
		for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				turkish = l == 1;
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
					printf("FAIL case %u (%s): crashed\n", i, l ? "tr" : "en");
			}
		}
	}
	printf("Plus 3D waterfall UI: %d cases (English and Turkish), %d failed\n", cases, bad);
	return bad != 0;
}
