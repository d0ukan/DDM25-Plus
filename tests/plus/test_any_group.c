/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus r15 "channels" (docs/plus.md "Round 15", item 2): the RX group list "Any" in the real DMR call layer
 * (built with CONFIG_DMR_ANY=1 CONFIG_CHAN_EXTRAS=1 inside upstream's call test fixture, its main renamed). Any is
 * dmr_listen_filter_t.any_group, no longer digital monitor (hear_all): every group call on the channel's slot plays,
 * a private call between two other stations does not (digital monitor still plays it), a private call to us plays
 * either way; without either a group call to another talkgroup stays out, as before. */
#define main upstream_call_main
#include "dmrCallTest.c"
#undef main

static int ag_fails;
#define ACHECK(c) do { if (!(c)) { ag_fails++; printf("FAIL any group %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* A whole DMO call with this LC: did it play? */
static bool plays(const dmr_call_config_t *c, uint8_t flco, uint32_t dst, uint32_t from)
{
	uint8_t lc[9];
	unsigned frame = 1;
	setup(c);
	mk_lc(lc, flco, dst, from);
	rx_call(lc, 1, true, 0, 2, true, &frame);
	run_ms(DMR_CALL_LBT_DMO_MS + 500);
	return sink_log.begins == 1;
}

static void any_group_tests(void)
{
	dmr_call_config_t c = base_cfg();
	/* The channel's own talkgroups only */
	ACHECK(plays(&c, DMR_FLCO_GROUP_VOICE, TG_TALK, 2345678));
	ACHECK(!plays(&c, DMR_FLCO_GROUP_VOICE, 2627, 2345678));
	ACHECK(!plays(&c, DMR_FLCO_UNIT_VOICE, 2345679, 2345678));
	/* Any: every talkgroup, not the private calls of others; ours still */
	c.listen.any_group = true;
	ACHECK(plays(&c, DMR_FLCO_GROUP_VOICE, 2627, 2345678));
	ACHECK(plays(&c, DMR_FLCO_GROUP_VOICE, 16777000u, 2345678));
	ACHECK(!plays(&c, DMR_FLCO_UNIT_VOICE, 2345679, 2345678));
	ACHECK(plays(&c, DMR_FLCO_UNIT_VOICE, OWN_ID, 2345678));
	/* Digital monitor (hear_all) on top: the private call between others too */
	c.listen.hear_all = true;
	ACHECK(plays(&c, DMR_FLCO_UNIT_VOICE, 2345679, 2345678));
	c.listen.any_group = false;
	ACHECK(plays(&c, DMR_FLCO_GROUP_VOICE, 2627, 2345678));
}

int main(void)
{
	int prior = upstream_call_main();
	any_group_tests();
	printf("Plus RX group list Any (every talkgroup, not digital monitor): %d failures\n", ag_fails);
	return prior != 0 || ag_fails != 0;
}
