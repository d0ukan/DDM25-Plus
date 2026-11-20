/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM25 Plus one-key SOS in the side key assignment (core/input/keyassign.c): KEYFN_SOS is appended (stored values
 * never move), may go on a press and a long press, sends ACT_SOS; set on the press with the long press left at
 * Default, the long press sends ACT_SOS too (holding the key confirms its page); a long press of its own stays. Not
 * assigned by default. Without CONFIG_SOS_KEY a stored SOS reads as Default. Built with -DCONFIG_KEY_ASSIGN=1 and
 * -DCONFIG_SOS_KEY=1 / 0. */
#include <stdio.h>
#include <string.h>
#include "input/keyassign.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
	/* appended after the last upstream function */
	CHECK(KEYFN_SOS == KEYFN_AB + 1 && KEYFN_COUNT == KEYFN_SOS + 1 && KEYFN_AB == 16);
	CHECK(ACT_SOS == ACT_AB + 1 && ACT_COUNT == ACT_SOS + 1);

	static const keymap_entry_t base[] = {
		{ 1, ACT_MONITOR, ACT_NONE, true, false, 0 },	/* SK1: monitor (raw edges) */
		{ 2, ACT_AB, ACT_USER_2, false, false, 0 },	/* SK2 */
		{ 3, ACT_SELECT, ACT_SELECT, false, false, 0 },
	};
	static const uint8_t side[] = { 1, 2 };
	keymap_entry_t out[3];
	uint16_t slots[KEYASSIGN_SLOTS] = { 0 };

	/* Default: not assigned, the board's map as it is */
	CHECK(keyassign_get(slots, 0, false) == KEYFN_DEFAULT && keyassign_get(slots, 1, true) == KEYFN_DEFAULT);
	keyassign_build(base, 3, side, 2, slots, out);
	CHECK(memcmp(out, base, sizeof base) == 0);

#if CONFIG_SOS_KEY
	CHECK(keyassign_allowed(KEYFN_SOS, false) && keyassign_allowed(KEYFN_SOS, true));
	/* SK2's press: SOS; its long press left at Default sends SOS too */
	keyassign_set(slots, 1, false, KEYFN_SOS);
	CHECK(keyassign_get(slots, 1, false) == KEYFN_SOS && keyassign_get(slots, 1, true) == KEYFN_DEFAULT);
	keyassign_build(base, 3, side, 2, slots, out);
	CHECK(out[1].short_action == ACT_SOS && out[1].long_action == ACT_SOS && !out[1].raw_edges);
	CHECK(out[0].short_action == ACT_MONITOR && out[2].short_action == ACT_SELECT);
	/* a long press of its own stays */
	keyassign_set(slots, 1, true, KEYFN_SCAN);
	keyassign_build(base, 3, side, 2, slots, out);
	CHECK(out[1].short_action == ACT_SOS && out[1].long_action == ACT_SCAN);
	/* SOS on SK1's long press only (the monitor key): the press stays the monitor, the long press SOS */
	keyassign_set(slots, 0, true, KEYFN_SOS);
	keyassign_build(base, 3, side, 2, slots, out);
	CHECK(out[0].short_action == ACT_MONITOR && out[0].long_action == ACT_SOS);
	/* SOS on a press of the monitor key: a press of its own */
	keyassign_set(slots, 0, false, KEYFN_SOS);
	keyassign_set(slots, 0, true, KEYFN_DEFAULT);
	keyassign_build(base, 3, side, 2, slots, out);
	CHECK(out[0].short_action == ACT_SOS && out[0].long_action == ACT_SOS && !out[0].raw_edges);
#else
	CHECK(!keyassign_allowed(KEYFN_SOS, false) && !keyassign_allowed(KEYFN_SOS, true));
	slots[0] = KEYASSIGN_MARKER;
	slots[2] = KEYFN_SOS | (KEYFN_SOS << 8);	/* stored by a build with it */
	CHECK(keyassign_get(slots, 1, false) == KEYFN_DEFAULT && keyassign_get(slots, 1, true) == KEYFN_DEFAULT);
	keyassign_build(base, 3, side, 2, slots, out);
	CHECK(out[1].short_action == ACT_AB && out[1].long_action == ACT_USER_2);
#endif
	printf("sos key: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
