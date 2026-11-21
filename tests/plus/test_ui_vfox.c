/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus: every mode in the VFO (CONFIG_VFO_MODES) and the multi-watch key (item 22), through the real UI in the
 * upstream simulator. Built like tests/plus/test_ui_dpmr.c with -DCONFIG_VFO_MODES=1 -DCONFIG_MULTI_WATCH=1 and every
 * digital mode, core/radio/vfox.c and core/dpmr/dpmr.c. The simulator's radio keeps each VFO's mode and parameters as
 * the radio core does (tools/plus_vfox.py: ui_svc_vfo_apply, ui_svc_mode, VFOs 2..5 for C..F); what the radio core does
 * with them is tests/plus/test_vfox_radio.inc's.
 * Flows: 145.575 MHz typed, Mode tile to P25, # to VFO settings, TG 99, NAC 456 in hex and in decimal, NAC / TG Any,
 * unit ID, Home's line; the whole cycle FM > DMR > P25 > YSF > NXDN > dPMR > AM > FM; YSF DG-ID, NXDN RAN, dPMR CC; DMR
 * color code / timeslot / every group Any; Save as new channel; the air band; the VFO row's # in Zones; multi-watch with
 * C, D, E, F each a VFO of its own in P25, YSF, NXDN, dPMR; the Dual/multi-watch key function. */
#define main upstream_ui_main
#include "uiFlowTest.c"
#undef main
#include "ui/ui_svc.h"
#include "radio/radio_nxdn.h"
#include "radio/radio_dpmr.h"
#include "p25/p25_lab.h"

uint32_t radio_nxdn_caller(radio_nxdn_caller_t *out)
{
	if (out)
		memset(out, 0, sizeof *out);
	return 0;
}

uint32_t radio_dpmr_caller(radio_dpmr_caller_t *out)
{
	if (out)
		memset(out, 0, sizeof *out);
	return 0;
}

size_t radio_dpmr_caller_line(char *out, size_t cap)
{
	if (out && cap)
		out[0] = '\0';
	return 0;
}

uint32_t p25_lab_ysf_caller(char src[11], char dst[11])
{
	if (src)
		src[0] = '\0';
	if (dst)
		dst[0] = '\0';
	return 0;
}

/* Into the VFO (Zones > VFO), 145.575 MHz typed, Mode tile twice: DMR, P25 */
#define VFO "ok 4 up ok 1 4 5 5 7 5 ok "
#define P25 VFO "quick 4 4 "
/* VFO settings (# in Quick): Mode, RX, TX, Talkgroup, NAC, P25 Radio ID, Save as new channel */
#define SET P25 "ctx "
#define TG SET "down down down "
#define NAC SET "down down down down "
/* multi-watch with 4 channels, C (# 6) set to its own VFO: the picker's zone list, its VFO row */
#define MW "mwatch:4 quick 7 "
#define MW6 "mwatch:6 quick 7 "
int main(void)
{
	const flow_t checks[] = {
		/* The Mode tile cycles every mode the build has */
		{ "VX mode tile: DMR", VFO "quick", "4", "vmode=DMR toast=VFO:_DMR", 1, false },
		{ "VX mode tile: P25", VFO "quick 4", "4", "vmode=P25 toast=VFO:_P25 freq=145575000 nac=F7E tg=0 p25id=3101276",
		  1, false },
		{ "VX mode tile: YSF NXDN dPMR", P25, "4 4 4", "vmode=dPMR toast=VFO:_dPMR", 3, false },
		{ "VX mode tile: FM after dPMR (no AM receiver)", P25, "4 4 4 4", "vmode=FM", 4, false },
		{ "VX mode tile: AM after dPMR", "amrx " P25, "4 4 4 4", "vmode=AM toast=VFO:_AM", 4, false },
		{ "VX mode tile: FM after AM", "amrx " P25, "4 4 4 4 4", "vmode=FM", 5, false },
		{ "VX AM on Home", "amrx " P25 "4 4 4 4 back", "", "shows=VFO_AM vmode=AM", 0, false },
		/* VFO settings: P25 rows, TG 99, NAC 456 hex, the decimal entry, Any */
		{ "VX settings open", P25, "ctx", "screen=vfo shows=VFO_settings shows=Talkgroup shows=NAC hides=TX_contact", 1, false },
		{ "VX TG 99", TG, "ok 9 9 ok", "tg=99 vmode=P25 rxonly=0", 4, false },	/* NAC Any: Classic r13 sends Same as RX as $293 */
		{ "VX NAC 1110", NAC, "ok 1 1 1 0 ok", "nac=456 shows=1110_($456)", 6, false },
		{ "VX NAC hex digits", NAC, "ok ctx 4 5 6 ok", "nac=456", 6, false },
		{ "VX TG 99 NAC 456: TX allowed", TG "ok 9 9 ok down ok 1 1 1 0 ok", "", "tg=99 nac=456 rxonly=0", 0, false },
		{ "VX NAC Any", NAC "ok 1 1 1 0 ok", "ok ctx ok", "nac=F7E rxonly=1", 3, false },
		{ "VX TG Any", TG "ok 9 9 ok", "ok ctx ok", "tg=0 rxonly=1", 3, false },
		{ "VX unit ID", SET "down down down down down", "ok 1 2 3 4 5 6 7 ok", "p25id=1234567", 9, false },
		{ "VX Home: VFO P25 NAC TG", TG "ok 9 9 ok down ok 1 1 1 0 ok back back", "", "screen=home shows=NAC_1110_TG_99", 0,
		  false },
		{ "VX Home: Any", SET "back back", "", "shows=NAC_Any_TG_Any", 0, false },
		{ "VX long OK says it is in effect", SET, "long:ok", "toast=VFO:_changes_apply_at_once writes=0", 1, false },
		/* The Mode row in VFO settings: the same cycle */
		{ "VX settings Mode row: YSF", SET, "ok", "vmode=YSF shows=DG-ID hides=NAC", 1, false },
		{ "VX YSF DG-ID 12", SET "ok down down down", "ok 1 2 ok", "vmode=YSF tg=12 rxonly=1", 4, false },
		{ "VX NXDN RAN 5", SET "ok ok down down down", "ok 5 ok", "vmode=NXDN tg=5 shows=RAN", 3, false },
		{ "VX dPMR CC 7", SET "ok ok ok down down down", "ok 7 ok", "vmode=dPMR tg=8 shows=CC", 3, false },
		{ "VX Home: dPMR CC", SET "ok ok ok down down down ok 7 ok back back", "", "shows=dPMR_CC_7", 0, false },
		/* DMR Any in the VFO: color code, timeslot, every group */
		{ "VX DMR rows", VFO "quick 4 ctx", "", "screen=vfo shows=Color_code shows=Timeslot shows=RX_group_list", 0,
		  false },
		{ "VX DMR CC Any", VFO "quick 4 ctx down down down", "ok down down ok", "vcc=16 toast=Any_color_code:_RX_only",
		  4, false },
		{ "VX DMR TS Any", VFO "quick 4 ctx down down down down", "ok ok", "vts=3", 2, false },
		{ "VX DMR every group", VFO "quick 4 ctx down down down down down", "ok", "listany=1 shows=Any_(all_groups)", 1,
		  false },
		/* Save as new channel: the channel editor on a copy of the P25 VFO, saved as a channel */
		{ "VX save as new channel", TG "ok 9 9 ok down ok 1 1 1 0 ok down down",
		  "ok", "screen=channel shows=P25 shows=$456", 1, false },
		{ "VX save as new channel writes", TG "ok 9 9 ok down ok 1 1 1 0 ok down down ok", "long:ok",
		  "writes=1 toast=Saved", 1, true },
		/* Zones > the VFO row's #: its settings */
		{ "VX zones VFO row", P25 "back", "ok 4 ctx", "screen=vfo", 3, false },
		/* Multi-watch: C its own VFO (VFO 2) with the VFO key on the selected letter, or from # 6's picker (its zone
		 * list's VFO row); P25 on it with TG 99 NAC 456; A stays a channel; C keeps its VFO when selected again */
		{ "VX MW C own VFO by the VFO key", MW "abkey abkey", "vfokey tune:c", "mwsel=C vfo=1 vfoidx=2 toast=C:_VFO", 1, false },
		{ "VX MW C own VFO from the picker", MW, "ctx 6 up up up up up ok up ok", "mwposc=3 mwsel=A vfo=0", 10, false },
		{ "VX MW C from the picker, selected", MW "ctx 6 up up up up up ok up ok", "abkey abkey tune:c", "mwsel=C vfo=1 vfoidx=2",
		  2, false },
		{ "VX MW C in P25", MW "abkey abkey vfokey", "quick 4 4 ctx down down down ok 9 9 ok down ok 1 1 1 0 ok",
		  "vfoidx=2 vmode=P25 tg=99 nac=456", 18, false },
		{ "VX MW A stays a channel", MW "abkey abkey vfokey quick 4 4 back abkey abkey tune:a", "", "mwsel=A vfo=0", 0, false },
		{ "VX MW C keeps its P25 VFO", MW "abkey abkey vfokey quick 4 4 back abkey abkey abkey abkey tune:c", "",
		  "mwsel=C vfo=1 vfoidx=2 vmode=P25", 0, false },
		/* Six letters: D, E, F each its own VFO in YSF, NXDN, dPMR */
		{ "VX MW D E F", MW6 "abkey abkey abkey vfokey quick 4 4 4 back abkey vfokey quick 4 4 4 4 back abkey vfokey",
		  "quick 4 4 4 4 4", "mwsel=F vfoidx=5 vmode=dPMR", 6, false },
		{ "VX MW D is YSF, E NXDN", MW6 "abkey abkey abkey vfokey quick 4 4 4 back abkey vfokey quick 4 4 4 4 back abkey "
		  "vfokey quick 4 4 4 4 4 back abkey abkey abkey abkey tune:d", "", "mwsel=D vfoidx=3 vmode=YSF", 0, false },
		{ "VX MW E is NXDN", MW6 "abkey abkey abkey vfokey quick 4 4 4 back abkey vfokey quick 4 4 4 4 back abkey "
		  "vfokey quick 4 4 4 4 4 back abkey abkey abkey abkey abkey tune:e", "", "mwsel=E vfoidx=4 vmode=NXDN", 0, false },
		/* Item 22: the side key function, multi-watch on and off with one key */
		{ "VX key: multi-watch on", "mwatch:4", "dualkey", "dual=1 mwn=4", 1, false },
		{ "VX key: multi-watch off", "mwatch:4 dualkey", "dualkey", "dual=0", 1, false },
		{ "VX key function named", "", "ok 7 8 down down down ok ok ok ok ok ok ok ok ok ok ok ok ok", "shows=Dual/multi-watch", 19, false },
	};
	for (unsigned i = 0; i < sizeof checks / sizeof checks[0]; i++)
		run_flow(&checks[i], sim_geom_find("240x320"), SIM_MAP_K, "Plus DM32");
	printf("Plus every mode in the VFO, UI: %d flows, %d failures\n", flows_run, fails);
	return fails != 0;
}
