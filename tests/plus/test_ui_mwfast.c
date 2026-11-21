/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus round 15 item 3, the faster multi-watch ring (CONFIG_MWATCH_FAST, docs/plus.md "The multi-watch ring"),
 * through the real UI in upstream's simulator, on the DM-32UV's display class (240 x 320, insets 10 x 5), in English
 * and in Turkish. Home's ring line follows the radio core's estimate (radio_mwatch.h radio_mw_ring_ms): with 3..6
 * channels a quiet FM-type stop takes 50 ms and a DMR one 100 ms; a P25 channel's extra is the hold alone (no wait);
 * Watch channels 2 stay dual watch's panes, with no ring line. Each case runs in a child process. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_nav.h"
#include "ui/i18n.h"
#include "radio/radio_mwatch.h"

static int fails;
static const char *name;
static bool turkish;

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

static void layout(void)
{
	char issues[SIM_LAYOUT_MAX][160];
	run("wait:200");
	int n = sim_layout_issues(issues, SIM_LAYOUT_MAX);
	for (int i = 0; i < n; i++)
		printf("FAIL %s (%s): layout: %s\n", name, turkish ? "tr" : "en", issues[i]);
	fails += n;
}

static void start(const char *n)
{
	name = n;
	sim_reset(sim_geom_find("240x320i10x5"), SIM_MAP_K);
	if (getenv("SCAN_SHOTS") != NULL)
		sim_set_shot_dir(getenv("SCAN_SHOTS"), turkish ? "mw-tr-" : "mw-en-");
	if (turkish && !lang_set(lang_find('t' << 8 | 'r'))) {
		printf("FAIL %s: no Turkish in this build\n", name);
		fails++;
	}
	run("wait:200");
}

/* "Ring 0.3 s" for ms */
static void expect_ring(uint32_t ms)
{
	char e[64];
	uint32_t tenths = (ms + 50u) / 100u;
	snprintf(e, sizeof e, "expect:shows=Ring_%u.%u_s", (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
	run(e);
}

/* The simulator's zone from position 3: COUNTY DMR, County fire (FM), Metro DMR, Hill top, North link (DMR), Harbor (FM) */
static void ring4(void)
{
	start("ring of 4");
	run("mwatch:4 quick 7 wait:500");
	run("expect:dual=1 expect:mwn=4 shot:ring4");
	expect_ring(radio_mw_ring_ms(4, 3));	/* three DMR channels */
	layout();
}

static void ring6(void)
{
	start("ring of 6");
	run("mwatch:6 quick 7 wait:500");
	run("expect:mwn=6 shot:ring6");
	expect_ring(radio_mw_ring_ms(6, 4));	/* four DMR channels (North link too), two FM */
	layout();
}

static void pair(void)
{
	start("dual watch keeps its panes");
	run("mwatch:2 quick 7 wait:500");
	run("expect:dual=1 expect:mwn=0 expect:hides=Ring_");
	layout();
}

typedef void (*case_fn)(void);
static const case_fn all[] = { ring4, ring6, pair };

int main(void)
{
	int bad = 0, cases = 0;
	printf("Plus faster ring estimates: 4 channels (3 DMR) %u ms, 6 channels (4 DMR) %u ms, a P25 one +%u ms\n",
	       (unsigned)radio_mw_ring_ms(4, 3), (unsigned)radio_mw_ring_ms(6, 4),
	       (unsigned)radio_mw_p25_extra_ms(6, RADIO_DUAL_DELAY_MS));
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
	printf("Plus faster ring UI: %d cases (English and Turkish), %d failed\n", cases, bad);
	return bad != 0;
}
