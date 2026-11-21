/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus last calls, the portable part (core/ui/lastcall.c; tools/test_xband.py builds it with
 * -DCONFIG_LAST_CALLS=1): one entry a call, newest first; a call going on updates its entry (its source once known,
 * the time), a pause of LASTCALL_GAP_MS or more makes the same station's next transmission a call of its own; a new
 * call naming the same known source within the gap is the same call (a caller reported twice), one whose source is
 * not known yet never is (a P25 reply on the same talkgroup); the letter, the mode, the private flag and the
 * destination tell stations apart; the oldest drops out when the list is full; texts (YSF callsigns, DTMF strings)
 * packed and back, '*' and '#' kept; the 16-byte entry. */
#include <stdio.h>
#include <string.h>
#include "ui/lastcall.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static lastcall_t key(uint8_t mode, char letter, uint32_t id, uint32_t tg, uint8_t flags)
{
	lastcall_t k;
	memset(&k, 0, sizeof k);
	k.mode = mode;
	k.letter = (uint8_t)letter;
	k.id = id;
	k.tg = tg;
	k.flags = flags;
	return k;
}

static void test_calls(void)
{
	lastcall_list_t l;
	lastcall_clear(&l);
	CHECK(l.n == 0, "empty");
	lastcall_t a = key(1, 'B', 3101234, 2862, 0), b = key(2, 'C', 77285, 101, 0);
	lastcall_note(&l, &a, true, 1000);
	lastcall_note(&l, &b, true, 1500);
	CHECK(l.n == 2 && l.e[0].mode == 2 && l.e[1].letter == 'B', "two calls, newest first");
	/* B's DMR call goes on (heard again): its entry, moved up, the time updated */
	lastcall_note(&l, &a, false, 3000);
	CHECK(l.n == 2 && l.e[0].id == 3101234 && l.e[0].ms == 3000 && l.e[1].id == 77285, "a call going on moves up");
	/* quiet for the gap, then heard again: another call of the same station */
	lastcall_note(&l, &a, false, 3000 + LASTCALL_GAP_MS);
	CHECK(l.n == 3 && l.e[0].ms == 3000 + LASTCALL_GAP_MS && l.e[1].ms == 3000, "after the gap: a call of its own");
	/* reported twice as new within the gap with the same source: one call */
	lastcall_note(&l, &a, true, 9000);
	CHECK(l.n == 3 && l.e[0].ms == 9000, "a caller reported twice is one call");
}

static void test_p25_source(void)
{
	lastcall_list_t l;
	lastcall_clear(&l);
	/* a P25 call starts with its source unknown, the source comes with its first LDU1 */
	lastcall_t s0 = key(2, 0, 0, 101, 0), s1 = key(2, 0, 77285, 101, LASTCALL_ENC);
	lastcall_note(&l, &s0, true, 100);
	lastcall_note(&l, &s1, false, 400);
	CHECK(l.n == 1 && l.e[0].id == 77285 && (l.e[0].flags & LASTCALL_ENC) && l.e[0].ms == 400,
	      "the source and the flags filled in");
	/* the reply on the same talkgroup 2 s later: its source unknown at first, a call of its own */
	lastcall_t r0 = key(2, 0, 0, 101, 0), r1 = key(2, 0, 3105555, 101, 0);
	lastcall_note(&l, &r0, true, 2400);
	CHECK(l.n == 2 && l.e[0].id == 0 && l.e[1].id == 77285, "a reply is another call");
	lastcall_note(&l, &r1, false, 2600);
	CHECK(l.n == 2 && l.e[0].id == 3105555 && l.e[1].id == 77285, "the reply's source fills the reply's entry");
	/* a call going on with another source does not take over an entry with a known one */
	lastcall_t o = key(2, 0, 444, 101, 0);
	lastcall_note(&l, &o, false, 2700);
	CHECK(l.n == 3 && l.e[0].id == 444, "another known source: another call");
}

static void test_apart(void)
{
	lastcall_list_t l;
	lastcall_clear(&l);
	lastcall_t base = key(1, 'A', 3101234, 91, 0);
	lastcall_note(&l, &base, true, 100);
	lastcall_t v[] = {
		key(1, 'B', 3101234, 91, 0),			/* another letter */
		key(1, 0, 3101234, 91, 0),			/* no watch */
		key(4, 'A', 3101234 & 0xFFFF, 91, 0),		/* another mode */
		key(1, 'A', 3101234, 91, LASTCALL_PRIV),	/* a private call to unit 91 */
		key(1, 'A', 3101234, 92, 0),			/* another talkgroup */
	};
	for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++)
		lastcall_note(&l, &v[i], false, 200 + i);
	CHECK(l.n == 6, "six stations apart: %u", (unsigned)l.n);
	/* full: the oldest goes */
	for (unsigned i = 0; i < 4; i++) {
		lastcall_t k = key(5, 'D', 1000 + i, 7, 0);
		lastcall_note(&l, &k, true, 300 + i);
	}
	CHECK(l.n == LASTCALL_MAX && l.e[0].id == 1003 && l.e[LASTCALL_MAX - 1].tg == 91 &&
	      l.e[LASTCALL_MAX - 1].letter == 0 && l.e[LASTCALL_MAX - 1].mode == 1, "eight kept, the oldest two dropped");
	for (unsigned i = 0; i < l.n; i++)
		CHECK(l.e[i].letter != 'B', "B's call dropped");
	for (unsigned i = 0; i < l.n; i++)
		CHECK(!(l.e[i].letter == 'A' && l.e[i].tg == 91 && l.e[i].mode == 1 && l.e[i].flags == 0),
		      "the first call dropped");
}

static void test_text(void)
{
	lastcall_t e;
	char out[LASTCALL_CHARS + 1];
	memset(&e, 0, sizeof e);
	lastcall_pack("ta2dou", &e);
	lastcall_unpack(&e, out);
	CHECK(strcmp(out, "TA2DOU") == 0 && (e.flags & LASTCALL_TEXT), "a callsign: %s", out);
	memset(&e, 0, sizeof e);
	lastcall_pack("*1234#A9D", &e);
	lastcall_unpack(&e, out);
	CHECK(strcmp(out, "*1234#A9") == 0, "DTMF, the first 8: %s", out);
	memset(&e, 0, sizeof e);
	lastcall_pack("A.B", &e);
	lastcall_unpack(&e, out);
	CHECK(strcmp(out, "A?B") == 0, "others as ?: %s", out);
	/* a text station and a numbered one with the same bits are apart (the flag) */
	lastcall_list_t l;
	lastcall_clear(&l);
	lastcall_t t = key(3, 0, 0, 0, 0);
	lastcall_pack("TA2DOU", &t);
	lastcall_t n = key(3, 0, t.id, t.tg, 0);
	lastcall_note(&l, &t, true, 10);
	lastcall_note(&l, &n, false, 20);
	CHECK(l.n == 2, "a text is not a number");
	CHECK(sizeof(lastcall_t) == 16 && sizeof(lastcall_list_t) <= 16 * LASTCALL_MAX + 4, "16 bytes an entry");
}

int main(void)
{
	test_calls();
	test_p25_source();
	test_apart();
	test_text();
	printf("last calls: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
