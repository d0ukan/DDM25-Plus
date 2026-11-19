/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus Tools > Tone finder and Tools > DTMF / SelCall (integration/radiofoundry/fmdec/ui_fmdec.c) through the
 * real UI in the upstream simulator, the radio side (radio_msg.h, radio_fmdec.h) a fake set by each check. Built like
 * tests/classic/test_ui.c with -DCONFIG_P25_CHANNELS=1 -DCONFIG_MSG_RX=1 -DCONFIG_TONE_FIND=1 -DCONFIG_SELCALL=1 and
 * core/ui/ui_msg.c, ui_fmdec.c, core/radio/tonefind.c. Channel 4 (County fire) is FM, channel 3 (the one in use at
 * the start) DMR. The tone found is written into channel 4 through the channel editor's write (writes=1, the stored
 * channel's tones read back), into the VFO as its tone; the decoders listen only while their screen is on top. */
#define main upstream_ui_main
#include "uiFlowTest.c"
#undef main
#include "ui/ui_svc.h"
#include "codeplug/codeplug.h"
#include "radio/radio_msg.h"
#include "radio/radio_fmdec.h"
#include "radio/selcall.h"

/* --- the radio side --- */
static uint8_t wanted;
static radio_tonef_info_t ti;
static radio_selcall_t sc[RADIO_SELCALL_N];
static uint8_t sc_n, sc_only;
static uint32_t fseq;

void radio_msg_want(uint8_t kind) { wanted = kind; }
uint8_t radio_msg_status(void) { return wanted ? RADIO_MSG_ST_ON : RADIO_MSG_ST_OFF; }
uint32_t radio_msg_seq(void) { return 0; }
uint8_t radio_msg_count(uint8_t kind) { (void)kind; return 0; }
const radio_pager_msg_t *radio_msg_pager(uint8_t i) { (void)i; return NULL; }
const radio_acars_msg_t *radio_msg_acars(uint8_t i) { (void)i; return NULL; }
uint32_t radio_fmdec_seq(void) { return fseq; }
void radio_tonef_info(radio_tonef_info_t *out) { *out = ti; }
uint8_t radio_selcall_count(void) { return sc_n; }
bool radio_selcall_get(uint8_t i, radio_selcall_t *out)
{
	if (i >= sc_n)
		return false;
	*out = sc[sc_n - 1u - i];
	return true;
}
void radio_selcall_set(uint8_t only) { sc_only = only; }
uint8_t radio_selcall_only(void) { return sc_only; }
const char *selcall_set_name(uint8_t set)
{
	static const char *const names[] = { "SelCall", "ZVEI1", "ZVEI2", "CCIR", "EIA", "ZVEI", "DTMF" };
	return set <= SELCALL_DTMF ? names[set] : names[0];
}

static void heard(uint8_t state, uint8_t type, uint16_t value, uint8_t last_type, uint16_t last_value)
{
	memset(&ti, 0, sizeof ti);
	ti.state = state;
	ti.type = type;
	ti.value = value;
	ti.quality = 80;
	ti.last_type = last_type;
	ti.last_value = last_value;
	ti.chip_ok = true;
	ti.chip[0] = 0x0723;	/* 88.5 Hz in the 0x07 scale, as the BK4819 would put it */
	ti.chip[1] = 0x8000;
	fseq++;
}

/* Tools: the last two before Battery (the first Up from the top lands on Battery, the last row) */
#define TONEF "ok 6 up up up ok "
#define SELCALL "ok 6 up up ok "

static void check(bool ok, const char *what)
{
	flows_run++;
	if (!ok) {
		printf("FAIL %s\n", what);
		fails++;
	}
}

static uint32_t script_writes;	/* codeplug writes before the keys */

static bool script(const char *setup, const char *keys)
{
	char err[200];
	sim_reset(sim_geom_find("240x320"), SIM_MAP_K);
	bool ok = sim_run(setup, err, sizeof err);
	script_writes = sim_rec.codeplug_writes;
	if (!ok || !sim_run(keys, err, sizeof err)) {
		printf("FAIL script %s / %s: %s\n", setup, keys, err);
		fails++;
		return false;
	}
	return true;
}

int main(void)
{
	const sim_geom_t *g = sim_geom_find("240x320");
	heard(RADIO_TONEF_FOUND, TONE_CTCSS, 885, TONE_CTCSS, 885);
	const flow_t found[] = {
		{"Plus tone finder opens", "up", TONEF, "screen=tonefinder shows=CTCSS_88.5_Hz shows=FD6818:_CTCSS_88.5", 6,
		 false},
		{"Plus tone finder: OK opens Write tone", "up " TONEF, "ok", "screen=tonewrite shows=CTCSS_88.5_Hz_>", 1, false},
		{"Plus tone finder: RX and TX into the channel", "up " TONEF, "ok ok", "screen=tonefinder writes=1 "
		 "toast=Saved_channel_4", 2, true},
		{"Plus tone finder: on a DMR channel", "", TONEF "ok ok", "toast=FM_only:_this_is_not_an_FM_channel writes=0",
		 8, false},
		{"Plus tone finder: back to Tools", "up " TONEF, "back", "screen=tools", 1, false},
	};
	for (unsigned i = 0; i < sizeof found / sizeof found[0]; i++)
		run_flow(&found[i], g, SIM_MAP_K, "Plus DM32");

	/* what was written: the stored channel 4 with both tones, the rest as it was */
	channel_t before, after;
	check(codeplug_channel(4, &before), "channel 4 read");
	if (script("up", TONEF "ok ok") && codeplug_channel(4, &after)) {
		check(after.rx_tone.type == TONE_CTCSS && after.rx_tone.value == 885, "RX tone written");
		check(after.tx_tone.type == TONE_CTCSS && after.tx_tone.value == 885, "TX tone written");
		check(after.rx_hz == before.rx_hz && after.mode == before.mode && strcmp(after.name, before.name) == 0,
		      "the rest of the channel unchanged");
	}
	/* RX only, from the last tone found (a DCS code, the signal gone) */
	heard(RADIO_TONEF_NONE, TONE_NONE, 0, TONE_DCS, 0023);
	if (script("up", TONEF "ok down ok") && codeplug_channel(4, &after)) {
		check(after.rx_tone.type == TONE_DCS && after.rx_tone.value == 0023, "RX only: DCS 023N written");
		check(after.tx_tone.type == before.tx_tone.type && after.tx_tone.value == before.tx_tone.value,
		      "RX only: TX tone kept");
		check(sim_rec.codeplug_writes == script_writes + 1u, "RX only: one write");
	}
	const flow_t last[] = {
		{"Plus tone finder: the last tone shown", "up", TONEF, "shows=No_tone shows=Last:_DCS_023N", 6, false},
		{"Plus tone finder: the last tone to write", "up " TONEF, "ok", "screen=tonewrite shows=DCS_023N_>", 1, false},
	};
	for (unsigned i = 0; i < sizeof last / sizeof last[0]; i++)
		run_flow(&last[i], g, SIM_MAP_K, "Plus DM32");
	/* the equivalent DCS names of what is heard */
	heard(RADIO_TONEF_FOUND, TONE_DCS, 0023, TONE_DCS, 0023);
	const flow_t same[] = {
		{"Plus tone finder: DCS and its equivalents", "up", TONEF, "shows=DCS_023N shows=DCS_047I", 6, false},
	};
	run_flow(&same[0], g, SIM_MAP_K, "Plus DM32");
	/* nothing found yet */
	heard(RADIO_TONEF_SEARCH, TONE_NONE, 0, TONE_NONE, 0);
	const flow_t none[] = {
		{"Plus tone finder: searching", "up", TONEF, "shows=Searching...", 6, false},
		{"Plus tone finder: nothing to write yet", "up " TONEF, "ok", "screen=tonefinder toast=No_tone_found_yet", 1,
		 false},
	};
	for (unsigned i = 0; i < sizeof none / sizeof none[0]; i++)
		run_flow(&none[i], g, SIM_MAP_K, "Plus DM32");
	/* the VFO: its tone set, nothing written */
	heard(RADIO_TONEF_FOUND, TONE_CTCSS, 1000, TONE_CTCSS, 1000);
	if (script("up quick 4 wait:1000", TONEF "ok down down ok")) {
		const radio_view_t *v = sim_view();
		check(v->vfo, "in the VFO");
		check(v->ch.tx_tone.type == TONE_CTCSS && v->ch.tx_tone.value == 1000, "VFO TX tone set");
		check(v->ch.rx_tone.type != TONE_CTCSS || v->ch.rx_tone.value != 1000, "VFO RX tone not set (TX only)");
		check(sim_rec.codeplug_writes == script_writes, "VFO: no codeplug write");
	}
	/* the decoders listen only while their screen is on top */
	if (script("up", TONEF))
		check(wanted == RADIO_MSG_TONEF, "the tone finder listens");
	if (script("up", TONEF "ok"))
		check(wanted == RADIO_MSG_TONEF, "Write tone keeps it listening");
	if (script("up", TONEF "back back"))
		check(wanted == 0, "Back: the tone finder stops");

	/* DTMF / SelCall */
	sc_n = 2;
	sc[0] = (radio_selcall_t){ 1000, 446006250u, SELCALL_ZVEI, "12345" };
	sc[1] = (radio_selcall_t){ 2000, 446006250u, SELCALL_DTMF, "1234#" };
	const flow_t calls[] = {
		{"Plus SelCall opens", "up", SELCALL, "screen=selcall shows=DTMF_1234# shows=ZVEI_12345", 5, false},
		{"Plus SelCall: the newest first", "up", SELCALL, "focus=DTMF_1234#", 5, false},
		{"Plus SelCall: # picks the tone set", "up " SELCALL, "ctx", "toast=SelCall:_ZVEI1", 1, false},
		{"Plus SelCall: OK shows a row whole", "up " SELCALL, "ok", "toast=DTMF_1234#__446.00625", 1, false},
	};
	for (unsigned i = 0; i < sizeof calls / sizeof calls[0]; i++)
		run_flow(&calls[i], g, SIM_MAP_K, "Plus DM32");
	sc_only = SELCALL_ANY;
	if (script("up", SELCALL "ctx ctx ctx ctx"))
		check(sc_only == SELCALL_EIA, "# four times: EIA");
	sc_only = SELCALL_ANY;
	if (script("up", SELCALL "ctx ctx ctx ctx ctx"))
		check(sc_only == SELCALL_ANY, "# five times: Auto again");
	if (script("up", SELCALL))
		check(wanted == RADIO_MSG_SELCALL, "the SelCall decoder listens");
	sc_n = 0;
	const flow_t empty[] = {
		{"Plus SelCall: empty", "up", SELCALL, "shows=Nothing_heard_yet", 5, false},
	};
	run_flow(&empty[0], g, SIM_MAP_K, "Plus DM32");
	printf("Plus tone finder and DTMF / SelCall screens: %d flows, %d failures\n", flows_run, fails);
	return fails != 0;
}
