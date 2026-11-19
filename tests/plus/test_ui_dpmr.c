/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus channel editor: the dPMR mode and its colour code row (CONFIG_DPMR), through the real UI in the upstream
 * simulator. Built like tests/plus/test_ui_any.c with -DCONFIG_YSF=1 -DCONFIG_NXDN=1 -DCONFIG_DPMR=1 (Mode cycles
 * DMR > P25 > YSF > NXDN > dPMR > FM) and core/dpmr/dpmr.c. The receivers themselves are not in the simulator: the
 * UI's reads of them are stubbed here (nothing heard). */
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

/* the channel editor, Mode four times from DMR: dPMR; then the colour code row (where P25's talkgroup is) */
#define DPMR_EDIT "ok 5 ok down down ok ok ok ok "
#define DPMR_CC DPMR_EDIT "down down down down down down down "
int main(void)
{
	const flow_t checks[] = {
		{ "Plus dPMR mode", DPMR_EDIT, "", "shows=dPMR hides=NXDN", 0, false },
		/* a new dPMR channel hears any colour code */
		{ "Plus dPMR CC any", DPMR_CC, "", "shows=CC shows=Any hides=NAC", 0, false },
		/* code 42 typed shows 42 (stored as 43) and saves */
		{ "Plus dPMR CC typed", DPMR_CC, "ok 4 2 ok", "shows=42", 4, false },
		{ "Plus dPMR CC saves", DPMR_CC, "ok 4 2 ok long:ok", "writes=1 toast=Saved", 5, true },
		/* reopened: still dPMR, still 42 */
		{ "Plus dPMR CC kept", DPMR_CC "ok 4 2 ok long:ok long:back", "ok 5 ok down down down down down down down down down",
		  "shows=dPMR shows=42", 12, false },
		/* code 0 is a code, not "any" */
		{ "Plus dPMR CC 0", DPMR_CC, "ok 0 ok", "shows=CC hides=Any", 3, false },
		/* the context key while typing: any again */
		{ "Plus dPMR CC any by context", DPMR_CC "ok 4 2 ok", "ok ctx", "shows=Any hides=42", 2, false },
		/* past code 63: not saved */
		{ "Plus dPMR CC 64 refused", DPMR_CC, "ok 6 4 ok long:ok", "toast=CC_goes_up_to_63", 5, false },
		/* Mode once more: FM */
		{ "Plus dPMR then FM", DPMR_EDIT, "ok", "shows=FM hides=dPMR", 1, false },
	};
	for (unsigned i = 0; i < sizeof checks / sizeof checks[0]; i++)
		run_flow(&checks[i], sim_geom_find("240x320"), SIM_MAP_K, "Plus DM32");
	printf("Plus dPMR channel editor: %d flows, %d failures\n", flows_run, fails);
	return fails != 0;
}
