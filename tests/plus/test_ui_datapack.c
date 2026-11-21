/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus user data packs (docs/plus.md "User data packs") through the real UI in upstream's simulator
 * (tests/plus/ui_sim_dpk.c: the air items' simulator with a simulated data chip on the board), on the DM-32UV's display
 * class (240 x 320, insets 10 x 5), in English and in Turkish. Each case runs in a child process (the screens' static
 * state starts over); its packs are written as the radio writes them (dpk_begin / dpk_put / dpk_commit). Covers:
 *   - Settings > About with a RadioID pack in force: "RadioID pack: RadioID TR, 2026-11-20, 1 KB", "Air band pack:
 *     none"; a damaged one: "damaged, not used"; the page fits;
 *   - Tools > Airfields with an air band pack that names Guvercinlik (LTAB): the list line from the pack ("TWR 132.000
 *     +1": two towers in the pack, one in the table), its details list the pack's five frequencies (none of the
 *     table's alone), OK tunes a frequency only the pack has, airfields the pack doesn't name keep the table's;
 *   - air watch (# on the airfield's page) on the pack's five frequencies, its ATIS the last letter. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/i18n.h"
#include "ui/ui_svc.h"
#include "ui/airfield.h"
#include "storage/storage.h"
#include "storage/datapack.h"
#include "sim/sim_flash.h"
#include "ui_sim_dpk.h"

static int fails;
static const char *name;
static bool turkish;

#define T(en, tr) (turkish ? (tr) : (en))

static void check(bool c, const char *what)
{
	if (!c) {
		printf("FAIL %s (%s): %s\n", name, turkish ? "tr" : "en", what);
		fails++;
	}
}

static bool run(const char *script)
{
	char err[300];
	if (!sim_run(script, err, sizeof err)) {
		printf("FAIL %s (%s): %s: %s\n", name, turkish ? "tr" : "en", script, err);
		fails++;
		return false;
	}
	return true;
}

/* "a=b c=d": every check after the script, as tests/host/uiFlowTest.c's run_flow */
static void expect(const char *e)
{
	char buf[600], *o = buf;
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
	run(buf);
}

static void layout(void)
{
	char issues[SIM_LAYOUT_MAX][160];
	run("wait:200");
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int i = 0; i < n; i++)
		printf("FAIL %s (%s): layout: %s\n", name, turkish ? "tr" : "en", issues[i]);
	fails += n;
}

static void pos(double la, double lo)
{
	air_sim.pos = true;
	air_sim.lat_e7 = (int32_t)(la * 1e7 + (la < 0 ? -0.5 : 0.5));
	air_sim.lon_e7 = (int32_t)(lo * 1e7 + (lo < 0 ? -0.5 : 0.5));
}

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	const char *shots = getenv("DPK_SHOTS");	/* a directory: the "shot:" tokens save the screens there */
	if (shots != NULL)
		sim_set_shot_dir(shots, turkish ? "dpk-tr-" : "dpk-en-");
	dpk_sim_setup();
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	pos(39.9208, 32.8541);		/* Ankara, Kizilay: Guvercinlik (LTAB) the nearest */
	run("wait:200");
}

/* A pack written as the radio writes one, then the slots checked as at boot */
static void write_pack(uint8_t type, const uint8_t *p, uint32_t len, const char *pname)
{
	dpk_hdr_t h;
	dpk_writer_t w;
	memset(&h, 0, sizeof h);
	h.type = type;
	h.fmt = 1;
	h.len = len;
	h.crc = storage_crc32(0, p, len);
	memcpy(h.date, "20261120", 8);
	strncpy(h.name, pname, sizeof h.name);
	strncpy(h.tag, "test", sizeof h.tag);
	check(dpk_begin(&w, &h) && dpk_put(&w, 0, p, len) && dpk_commit(&w), "the pack written");
	dpk_scan();
}

/* A RadioID pack of n IDs (format 1: 15-byte records, 16 characters of 6 bits) */
static void radioid_pack(uint32_t n)
{
	static uint8_t p[8 + 15 * 100];
	memset(p, 0, sizeof p);
	p[0] = (uint8_t)n, p[1] = (uint8_t)(n >> 8), p[4] = 15, p[5] = 16;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t id = 2860001u + i;
		p[8 + 15 * i] = (uint8_t)id, p[9 + 15 * i] = (uint8_t)(id >> 8), p[10 + 15 * i] = (uint8_t)(id >> 16);
	}
	write_pack(DPK_RADIOID, p, 8 + 15 * n, "RadioID TR");
}

/* The air band pack: Guvercinlik (LTAB) with ATIS 126.300, GND 120.200, TWR 132.000, TWR 118.250, APP 119.500 (the
 * table has GND 120.200 and TWR 132.000 alone) */
static const uint32_t ltab_khz[5] = { 126300, 120200, 132000, 118250, 119500 };
static const uint8_t ltab_type[5] = { AIR_ATIS, AIR_GND, AIR_TWR, AIR_TWR, AIR_APP };

static void air_pack(void)
{
	uint8_t p[64];
	uint32_t n = 0;
	p[n++] = 1, p[n++] = 0, p[n++] = 0, p[n++] = 0;
	memcpy(p + n, "LTAB\0\0\0\0", 8), n += 8, p[n++] = 5;
	for (int k = 0; k < 5; k++) {
		uint16_t ch = (uint16_t)((ltab_khz[k] - AIR_CH_BASE_KHZ) / 5u);
		p[n++] = (uint8_t)ch, p[n++] = (uint8_t)(ch >> 8), p[n++] = ltab_type[k];
	}
	write_pack(DPK_AIR, p, n, "TR aerodromes");
}

/* Settings > About, scrolled to its end (the packs' lines are near it) */
static void about(void)
{
	run("ok 7 9 down ok");
	expect("screen=about");
	run("down down down down down down down down down down down down down down down down down down down down");
	run("down down down down down down down down down down down down down down down down down down down down");
}

/* --- Settings > About --- */

static void dp_about(void)
{
	start("DP About with a RadioID pack");
	radioid_pack(60);
	about();
	/* (the page draws the label dim, apart from its value, and wraps the value after the pack's name) */
	expect(T("screen=about shows=RadioID_pack: shows=RadioID_TR, shows=2026-11-20,_1_KB shows=Air_band_pack: shows=none",
		 "screen=about shows=RadioID_paketi: shows=TR,_2026-11-20,_1_KB shows=yok"));
	run("shot:about-packs");
	layout();
}

/* Both slots of the air band type damaged: About says so, the airfields answer from the table */
static void dp_damaged(void)
{
	start("DP damaged pack");
	air_pack();
	for (uint8_t s = 0; s < 2; s++)
		sim_flash_data[dpk_slot_addr(DPK_AIR, s) + DPK_HDR + 9] ^= 1u;	/* the payload under its CRC */
	dpk_scan();
	check(dpk_state(DPK_AIR) == DPK_BAD, "damaged");
	about();
	expect(T("screen=about shows=Air_band_pack: shows=damaged, shows=RadioID_pack: shows=none",
		 "screen=about shows=bozuk, shows=RadioID_paketi: shows=yok"));
	layout();
	ui_go_home();
	ui_push(SCR_AIRFIELDS, 0);
	run("wait:100");
	expect("shows=LTAB shows=TWR_132.000 hides=TWR_132.000_+");
}

/* --- Tools > Airfields with an air band pack --- */

static void dp_airfield(void)
{
	start("DP airfield from an air band pack");
	air_pack();
	ui_push(SCR_AIRFIELDS, 0);
	run("wait:100");
	expect("screen=airfields shows=LTAB shows=TWR_132.000_+1");	/* two towers in the pack */
	run("shot:airfields-pack");
	layout();
	run("down");			/* Etimesgut: not in the pack, the table's */
	expect("shows=LTAD shows=TWR_122.100");
	run("up ok");
	expect("shows=ATIS_126.300 shows=GND_120.200 shows=TWR_132.000 shows=TWR_118.250 shows=APP_119.500");
	run("shot:airfield-pack");
	layout();
	run("down down down");		/* ATIS, GND, TWR, TWR 118.250: only the pack has it */
	run("ok");
	expect("screen=home vfo=1 freq=118250000");
	layout();
}

#if CONFIG_AIR_WATCH
/* # on the airfield's page: the pack's five frequencies on the ring, its ATIS the last letter (E) */
static void dp_watch(void)
{
	start("DP air watch from an air band pack");
	air_pack();
	ui_push(SCR_AIRFIELDS, 0);
	run("wait:100");
	run("ok");
	expect(T("shows=#_watch_all", "shows=#_t"));
	run("ctx");
	expect(T("screen=home dual=1 mwn=5 mwradion=5 mwsel=A toast=LTAB:_Air_watch_on",
		 "screen=home dual=1 mwn=5 mwradion=5 mwsel=A toast=LTAB:_Hava_izleme_a"));
	check(air_sim.vfo_calls == 5, "five VFOs asked for");
	for (uint8_t k = 0; k < 5; k++) {
		uint32_t want = ltab_khz[(k + 1u) % 5u] * 1000u;	/* GND, TWR, TWR, APP, then the ATIS */
		check(air_sim.hz[k] == want && air_sim_vfo_hz(k) == want, "each letter on the pack's frequency, ATIS last");
	}
	check(air_sim.atis == 1u << 4, "only E marked as the ATIS");
	expect("shows=126.30");
	run("shot:home-air-pack");
	layout();
	run("back back wait:300");
	expect(T("dual=0 toast=Air_watch_off", "dual=0 toast=Hava_izleme_kapal"));
	check(air_sim.end_calls == 1, "the end asked for once");
}
#endif

typedef void (*case_fn)(void);
static const case_fn all[] = {
	dp_about, dp_damaged, dp_airfield,
#if CONFIG_AIR_WATCH
	dp_watch,
#endif
};

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
	printf("Plus data pack UI: %d cases (English and Turkish), %d failed\n", cases, bad);
	return bad != 0;
}
