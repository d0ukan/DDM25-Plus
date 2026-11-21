/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus separate RX and TX NAC (Classic r13's "RX NAC" / "TX NAC" rows with Plus's extras, CONFIG_P25_TXNAC,
 * docs/plus.md "Separate RX NAC and TX NAC"), through the real UI in the upstream simulator. Built like
 * tests/plus/test_ui_vfox.c (every digital mode, the VFO's modes, multi-watch) with -DCONFIG_P25_TXNAC=1.
 * Channel editor: the "RX NAC" row (hex, # Any, # # decimal), the "TX NAC" row: Same as RX by default, three hex
 * digits, # Same as RX, # # decimal, saved; RX Any keeps TX (Classic r13: an inherited TX NAC is then $293); an
 * explicit $F7E is refused on save; the row is P25's only (not YSF). VFO settings: the same row, the VFO's TX rule,
 * Home's line "NAC Any>$659" / "$293>$659", the unit ID row below it, Save as new channel carrying it, C's own VFO in
 * the multi-watch ring. */
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

/* A channel made P25 in the editor (NAC $293, TG 1, ID 1), the cursor on its Talkgroup row (tests/plus/test_ui_any.c) */
#define P25_EDIT "ok 5 ok down down ok "
#define P25_TG P25_EDIT "down down down down down down down "
#define RXNAC P25_TG "down "
#define TXNAC P25_TG "down down "
/* The VFO in P25 (tests/plus/test_ui_vfox.c), VFO settings: Mode, RX, TX, Talkgroup, RX NAC, TX NAC, ID, Save new */
#define VFO "ok 4 up ok 1 4 5 5 7 5 ok "
#define P25 VFO "quick 4 4 "
#define SET P25 "ctx "
#define TG SET "down down down "
#define VNAC SET "down down down down "
#define VTX SET "down down down down down "
#define MW "mwatch:4 quick 7 "
int main(void)
{
	const flow_t checks[] = {
		/* The rows */
		{ "NX rows", TXNAC, "", "shows=RX_NAC shows=659_($293) shows=TX_NAC shows=Same_as_RX", 0, false },
		/* TX NAC in hex, in decimal, back to Same as RX */
		{ "NX TX NAC typed", TXNAC, "ok 1 6 2 5 ok", "shows=1625_($659) shows=659_($293)", 6, false },
		{ "NX TX NAC while typed", TXNAC, "ok 1 6 2 5", "shows=[1625]_($659)", 5, false },
		{ "NX TX NAC opens on Same as RX", TXNAC, "ok", "shows=Same_as_RX", 1, false },
		/* # once is Same as RX, # again decimal: opened on Same as RX, the first # is decimal (as the RX NAC on Any) */
		{ "NX TX NAC hex digits", TXNAC, "ok ctx 6 5 9", "shows=$[6]59_(1625)", 5, false },
		{ "NX TX NAC hex digits stored", TXNAC, "ok ctx 6 5 9 ok", "shows=1625_($659)", 6, false },
		{ "NX TX NAC hex from a value", TXNAC "ok 2 9 3 ok", "ok ctx ctx 6 5 9 ok", "shows=1625_($659)", 8,
		  false },
		{ "NX TX NAC back to Same as RX", TXNAC "ok 1 6 2 5 ok", "ok ctx ok", "shows=Same_as_RX hides=1625", 3, false },
		/* Classic r13: an explicit TX NAC $F7E (3966) is not Same as RX: the channel is refused on save */
		{ "NX TX NAC 3966 refused", TXNAC "ok 1 6 2 5 ok", "ok 3 9 6 6 ok", "toast=TX_NAC_can't_be_3966 shows=[3966]_($F7E)",
		  6, false },
		{ "NX TX NAC 3966 not saved", TXNAC "ok 3 9 6 6 ok", "long:ok", "writes=0", 1, true },
		{ "NX TX NAC saves", TXNAC, "ok 1 6 2 5 ok long:ok", "writes=1 toast=Saved", 7, true },
		{ "NX TX NAC saved, reopened", TXNAC "ok 1 6 2 5 ok long:ok", "", "shows=1625_($659)", 0, true },
		/* The RX NAC row works as before */
		{ "NX RX NAC typed", RXNAC, "ok 2 9 3 ok", "shows=293_($125) shows=Same_as_RX", 5, false },
		{ "NX RX NAC hex digits", RXNAC, "ok ctx ctx 1 2 5 ok", "shows=293_($125)", 7, false },
		/* RX Any keeps TX (Classic r13: Same as RX then sends $293) */
		{ "NX RX NAC Any keeps TX", RXNAC, "ok ctx ok", "shows=Any_3966_($F7E) shows=Same_as_RX toast=none", 3, false },
		/* RX Any + TX $659 */
		{ "NX RX Any then TX NAC", RXNAC "ok ctx ok", "down ok 1 6 2 5 ok", "toast=none shows=1625_($659)", 7, false },
		{ "NX RX Any + TX NAC saves", RXNAC "ok ctx ok down ok 1 6 2 5 ok", "long:ok", "writes=1 toast=Saved", 1, true },
		{ "NX RX Any + TX NAC, TX back to Same", RXNAC "ok ctx ok down ok 1 6 2 5 ok", "ok ctx ok",
		  "toast=none shows=Same_as_RX", 3, false },
		{ "NX TX NAC first, then RX Any: keeps TX", TXNAC "ok 1 6 2 5 ok up", "ok ctx ok",
		  "shows=Any_3966_($F7E) shows=1625_($659) toast=none", 3, false },
		{ "NX TX NAC first, then RX Any: saves", TXNAC "ok 1 6 2 5 ok up ok ctx ok", "long:ok", "writes=1 toast=Saved",
		  1, true },
		/* RX only cleared by the TX NAC only with a talkgroup: TG Any first stays RX only */
		{ "NX TG Any, RX Any, TX NAC: still RX only", P25_TG "ok ctx ok down ok ctx ok down",
		  "ok 1 6 2 5 ok", "shows=1625 toast=none", 6, false },
		/* P25's only: not on YSF (the mode after P25) */
		{ "NX not on YSF", P25_EDIT "ok", "", "shows=DG-ID hides=TX_NAC", 0, false },
		/* The VFO */
		{ "NX VFO row", VTX, "", "screen=vfo shows=RX_NAC shows=TX_NAC shows=Same_as_RX", 0, false },
		{ "NX VFO TX NAC, RX Any, TG Any: listens", VTX, "ok 1 6 2 5 ok", "nac=F7E txnac=659 rxonly=1", 6, false },
		{ "NX VFO RX Any + TX $659 + TG 99: talks", TG "ok 9 9 ok down down ok 1 6 2 5 ok", "",
		  "nac=F7E txnac=659 tg=99 rxonly=0", 0, false },
		/* Home's row on the DM-32's width: both NAC forms ("NAC$456/1110 TG99"); with a TX NAC of its own even the tight
		 * form "NAC$293/659>$659/1625 TG99" does not fit next to the step, so the last resort keeps the decimals */
		{ "NX VFO Home: NAC Any>1625", TG "ok 9 9 ok down down ok 1 6 2 5 ok back back", "",
		  "screen=home shows=NACAny>1625_TG99", 0, false },
		{ "NX VFO Home: 659>1625", TG "ok 9 9 ok down ok 6 5 9 ok down ok 1 6 2 5 ok back back", "",
		  "shows=NAC659>1625_TG99 nac=293 txnac=659 rxonly=0", 0, false },
		{ "NX VFO Home: same NAC as before", TG "ok 9 9 ok down ok 1 1 1 0 ok back back", "",
		  "shows=NAC$456/1110_TG99 hides=>", 0, false },
		/* RX Any, Same as RX: Classic r13 sends $293, so the VFO with TG 99 still talks */
		{ "NX VFO TX NAC back to Same: talks with $293", TG "ok 9 9 ok down down ok 1 6 2 5 ok", "ok ctx ok",
		  "txnac=0 rxonly=0", 3, false },
		{ "NX VFO unit ID below", SET "down down down down down down", "ok 1 2 3 4 5 6 7 ok", "p25id=1234567", 9, false },
		{ "NX VFO save as new channel", TG "ok 9 9 ok down down ok 1 6 2 5 ok down down", "ok",
		  "screen=channel shows=P25 shows=1625_($659)", 1, false },
		{ "NX VFO save as new channel writes", TG "ok 9 9 ok down down ok 1 6 2 5 ok down down ok", "long:ok",
		  "writes=1 toast=Saved", 1, true },
		{ "NX VFO to YSF: no TX NAC", TG "ok 9 9 ok down down ok 1 6 2 5 ok up up up up up", "ok",
		  "vmode=YSF txnac=0 shows=DG-ID hides=Same_as_RX hides=$659", 1, false },
		/* Multi-watch: C's own VFO in P25 with a TX NAC */
		{ "NX MW C TX NAC", MW "abkey abkey vfokey",
		  "quick 4 4 ctx down down down ok 9 9 ok down ok 1 1 1 0 ok down ok 1 6 2 5 ok",
		  "vfoidx=2 vmode=P25 tg=99 nac=456 txnac=659 rxonly=0", 25, false },
	};
	for (unsigned i = 0; i < sizeof checks / sizeof checks[0]; i++)
		run_flow(&checks[i], sim_geom_find("240x320"), SIM_MAP_K, "Plus DM32");
	printf("Plus RX / TX NAC, UI: %d flows, %d failures\n", flows_run, fails);
	return fails != 0;
}
