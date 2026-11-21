// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 Open25-DM32 Contributors
// Check product identity and attribution through the existing simulator.
#define main upstream_ui_main
#include "uiFlowTest.c"
#undef main
#include "ui/ui_nav.h"

int main(void)
{
	char error[300];
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K); flows_run++;
	ui_push(SCR_CONFIRM, 0);
	if (!sim_run("wait:1 expect:shows=Factory_Reset expect:focus=No,_keep_settings ok expect:screen=home", error, sizeof error)) {
		printf("FAIL factory reset cancellation: %s\n", error); fails++;
	}
	char product[160];
	snprintf(product, sizeof product, "shows=%s", BUILD_PRODUCT_NAME);
	for (char *p = product; *p; p++) if (*p == ' ') *p = '_';
	char words[128] = BUILD_PRODUCT_NAME, about[256] = "";
	for (char *word = strtok(words, " "); word; word = strtok(NULL, " ")) {
		size_t used = strlen(about);
		snprintf(about + used, sizeof about - used, "%sshows=%s", used ? " " : "", word);
	}
	const flow_t identity[] = {
		{"Classic boot identity", "theme:light", "splash", product, 0, false},
		{"Classic About identity", "theme:light", "ok 7 up ok ok", about, 5, false},
		{"Classic About attribution", "theme:light ok 7 up ok ok", "down down down down down down", "shows=MIT shows=RadioFoundry shows=Authors", 6, false},
	};
	for (unsigned i = 0; i < sizeof identity / sizeof identity[0]; i++)
		run_flow(&identity[i], sim_geom_find("240x320i10x5"), SIM_MAP_K, "Classic");
	const flow_t short_boot = {"Classic short-display boot identity", "", "splash", product, 0, false};
	run_flow(&short_boot, sim_geom_find("128x64"), SIM_MAP_K, "Classic");
	const char *const home[] = {
		"", "rx:9991234,91,g", "pttdown wait:2500",
		"down", "down fmrx", "down pttdown wait:2500",
		"p25 dlmode:p25", "p25 dlmode:p25 p25rx:123456,91", "p25 dlmode:p25 pttdown wait:2500",
		"nxdn dlmode:nxdn", "nxdn dlmode:nxdn nxdnrx:12345,91", "nxdn dlmode:nxdn pttdown wait:2500",
	};
	for (unsigned i = 0; i < sizeof home / sizeof home[0]; i++) {
		const flow_t light = {"Classic Home light identity", "theme:light", home[i], "shows=Open25 shows=CLASSIC", 2, false};
		const flow_t dark = {"Classic Home dark identity", "theme:dark", home[i], "shows=Open25 shows=CLASSIC", 2, false};
		run_flow(&light, sim_geom_find("240x320i10x5"), SIM_MAP_K, "Classic");
		run_flow(&dark, sim_geom_find("240x320i10x5"), SIM_MAP_K, "Classic");
	}
	printf("Classic product: %d flows, %d failures\n", flows_run, fails);
	return fails != 0;
}
