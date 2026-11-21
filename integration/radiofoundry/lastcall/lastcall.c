/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus last calls (lastcall.h): the portable part. Installed as core/ui/lastcall.c. */
#include "config.h"
#if CONFIG_LAST_CALLS
#include <string.h>
#include "ui/lastcall.h"

_Static_assert(sizeof(lastcall_t) == 16, "a last call entry is 16 bytes");

static const char text_set[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/*#?";	/* 6 bits a character, 0 = space */

void lastcall_clear(lastcall_list_t *l)
{
	memset(l, 0, sizeof *l);
}

uint32_t lastcall_age_s(const lastcall_t *e, uint32_t now_ms)
{
	return (now_ms - e->ms) / 1000u;
}

bool lastcall_same(const lastcall_t *a, const lastcall_t *b)
{
	return a->mode == b->mode && a->letter == b->letter && a->tg == b->tg &&
	       (a->flags & (LASTCALL_PRIV | LASTCALL_TEXT)) == (b->flags & (LASTCALL_PRIV | LASTCALL_TEXT)) &&
	       (a->id == b->id || a->id == 0 || b->id == 0);
}

uint8_t lastcall_note(lastcall_list_t *l, const lastcall_t *k, bool new_call, uint32_t now_ms)
{
	/* A new call is the one going on only when it names the same source (a caller reported twice); one whose
	 * source is not known yet is always a call of its own (a P25 reply on the same talkgroup) */
	uint8_t i;
	for (i = 0; i < l->n; i++) {
		if (lastcall_same(&l->e[i], k) && (!new_call || (k->id != 0 && l->e[i].id == k->id)))
			break;
	}
	lastcall_t e;
	if (i < l->n && now_ms - l->e[i].ms < LASTCALL_GAP_MS) {
		e = l->e[i];	/* this call goes on */
		if (k->id != 0)
			e.id = k->id;
		e.flags = k->flags;
		e.nac = k->nac;
	} else {
		e = *k;
		if (l->n < LASTCALL_MAX)
			l->n++;
		i = (uint8_t)(l->n - 1u);	/* full: the oldest (the last) makes room */
	}
	e.ms = now_ms;
	memmove(&l->e[1], &l->e[0], i * sizeof l->e[0]);
	l->e[0] = e;
	return 0;
}

void lastcall_pack(const char *text, lastcall_t *e)
{
	uint64_t v = 0;
	for (unsigned c = 0; c < LASTCALL_CHARS; c++) {
		char ch = *text != '\0' ? *text++ : ' ';
		if (ch >= 'a' && ch <= 'z')
			ch = (char)(ch - 'a' + 'A');
		const char *at = strchr(text_set, ch);
		unsigned x = at != NULL && ch != '\0' ? (unsigned)(at - text_set) : sizeof text_set - 2u;
		v = v << 6 | x;
	}
	e->id = (uint32_t)(v >> 24) & 0xFFFFFFu;
	e->tg = (uint32_t)v & 0xFFFFFFu;
	e->flags |= LASTCALL_TEXT;
}

void lastcall_unpack(const lastcall_t *e, char out[LASTCALL_CHARS + 1])
{
	uint64_t v = (uint64_t)e->id << 24 | e->tg;
	for (unsigned c = LASTCALL_CHARS; c-- > 0;) {
		unsigned x = (unsigned)(v & 63u);
		out[c] = x < sizeof text_set - 1u ? text_set[x] : '?';
		v >>= 6;
	}
	unsigned n = LASTCALL_CHARS;
	while (n > 0 && out[n - 1u] == ' ')
		n--;
	out[n] = '\0';
}
#endif
