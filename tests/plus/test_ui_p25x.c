/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 extras in the call history (CONFIG_P25_EXTRAS with CONFIG_CALL_LOG and CONFIG_P25_CALLER), through
 * the real UI in the upstream simulator. Built like tests/plus/test_ui_any.c with -DCONFIG_CALL_LOG=1
 * -DCONFIG_P25_CALLER=1 -DCONFIG_P25_EXTRAS=1 -DCONFIG_DEV_TX=1. The P25 worker is not in the simulator:
 * p25_lab_call() (what the UI logs from) and the caller queries are stubbed here, one P25 call per flow
 * (tests/plus/test_p25x_worker.c checks them against the real worker). Each flow opens Recent and reads the call's
 * row, on the DM-32's screen (240x320, 10 px hidden left and right, the big font: narrow rows) and on a full 240x320
 * one: the label "P25 77285" stays whole, the value drops the NAC / "Private" / an emergency's talkgroup first, and
 * "EMERGENCY" stays. The rows' warning colour is upstream's UI_ITEM_WARN (not read back here). */
#define main upstream_ui_main
#include "uiFlowTest.c"
#undef main
#include "ui/ui_svc.h"
#include "p25/p25_lab.h"

static p25_lab_call_t stub_call;
uint32_t p25_lab_call(p25_lab_call_t *out)
{
	*out = stub_call;
	return out->n;
}
uint32_t p25_lab_p25_caller(uint16_t *tg, uint32_t *src)
{
	if (tg)
		*tg = stub_call.tg;
	if (src)
		*src = stub_call.src;
	return stub_call.n;
}
size_t p25_lab_p25_caller_line(char *out, size_t cap, bool pane)
{
	(void)pane;
	if (out && cap)
		out[0] = '\0';
	return 0;
}
unsigned p25_lab_p25_kind(uint32_t *target)
{
	if (target)
		*target = stub_call.target;
	return (stub_call.emergency ? (unsigned)P25_LAB_EMERGENCY : 0u) | (stub_call.priv ? (unsigned)P25_LAB_PRIVATE : 0u);
}

typedef struct {
	uint32_t src;
	uint16_t tg;
	bool priv;
	uint32_t target;
	bool emergency, encrypted;
	flow_t narrow, wide;	/* the DM-32's screen, a full 240x320 one */
} p25_case_t;

int main(void)
{
	static const p25_case_t cases[] = {
		/* an emergency: the word stays; its talkgroup where the row has room */
		{ 77285, 101, false, 0, true, false,
		  { "Plus P25 emergency in Recent (DM-32)", "wait:500", "ok 3", "shows=P25_77285 shows=EMERGENCY_1s hides=TG",
		    2, false },
		  { "Plus P25 emergency in Recent", "wait:500", "ok 3", "shows=P25_77285 shows=EMERGENCY_TG_101_1s", 2,
		    false } },
		/* unit to unit: "Private > 1234", then "> 1234" */
		{ 77285, 0, true, 1234, false, false,
		  { "Plus P25 unit to unit in Recent (DM-32)", "wait:500", "ok 3",
		    "shows=P25_77285 shows=>_1234_1s hides=Private hides=EMERGENCY hides=TG", 2, false },
		  { "Plus P25 unit to unit in Recent", "wait:500", "ok 3", "shows=P25_77285 shows=Private_>_1234_1s", 2,
		    false } },
		{ 3101234, 0, true, 3105678, true, false,
		  { "Plus P25 emergency unit to unit in Recent (DM-32)", "wait:500", "ok 3",
		    "shows=P25_3101234 shows=EMERGENCY_1s", 2, false },
		  { "Plus P25 emergency unit to unit in Recent", "wait:500", "ok 3", "shows=P25_3101234 shows=EMERGENCY_1s",
		    2, false } },
		/* a group call: the NAC goes first */
		{ 77285, 202, false, 0, false, false,
		  { "Plus P25 group call in Recent (DM-32)", "wait:500", "ok 3",
		    "shows=P25_77285 shows=TG_202_1s hides=NAC hides=EMERGENCY hides=Private", 2, false },
		  { "Plus P25 group call in Recent", "wait:500", "ok 3", "shows=P25_77285 shows=TG_202_$293/659_1s", 2,
		    false } },
		/* encrypted, as before the extras (on the DM-32 the label is shortened for it, as it was) */
		{ 77285, 202, false, 0, false, true,
		  { "Plus P25 encrypted group call in Recent (DM-32)", "wait:500", "ok 3", "shows=Encrypted_TG_202_1s", 2,
		    false },
		  { "Plus P25 encrypted group call in Recent", "wait:500", "ok 3",
		    "shows=P25_77285 shows=Encrypted_TG_202_1s", 2, false } },
	};
	uint32_t n = 100;
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
		for (unsigned g = 0; g < 2u; g++) {
			/* a new call number each time: the UI logs it as a new call */
			memset(&stub_call, 0, sizeof stub_call);
			stub_call.n = n++;
			stub_call.src = cases[i].src;
			stub_call.tg = cases[i].tg;
			stub_call.nac = 0x293;
			stub_call.clear = !cases[i].encrypted;
			stub_call.encrypted = cases[i].encrypted;
			stub_call.priv = cases[i].priv;
			stub_call.target = cases[i].target;
			stub_call.emergency = cases[i].emergency;
			stub_call.start_ms = stub_call.last_ms = 1;
			run_flow(g ? &cases[i].wide : &cases[i].narrow, sim_geom_find(g ? "240x320" : "240x320i10x5"),
				 SIM_MAP_K, "Plus DM32");
		}
	printf("Plus P25 extras in the call history: %d flows, %d failures\n", flows_run, fails);
	return fails != 0;
}
