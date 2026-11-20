/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus heard list (heard.h): the portable part. Installed as core/ui/heard.c. */
#include "config.h"
#if CONFIG_HEARD
#include <string.h>
#include "ui/heard.h"

_Static_assert(sizeof(heard_t) == 16, "a heard entry is 16 bytes");

static const char call_set[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/?";	/* 6 bits a character, 0 = space */

void heard_clear(heard_list_t *l)
{
	memset(l, 0, sizeof *l);
}

uint32_t heard_age_s(const heard_t *e, uint32_t now_s)
{
	return (now_s - e->last_s) & HEARD_CLOCK_MASK;
}

void heard_expire(heard_list_t *l, uint32_t now_s)
{
	uint8_t k = 0;
	for (uint8_t i = 0; i < l->n; i++) {
		if (heard_age_s(&l->e[i], now_s) < HEARD_AGE_MAX_S)
			l->e[k++] = l->e[i];
	}
	l->n = k;
}

void heard_note(heard_list_t *l, const heard_t *k, bool new_call, uint32_t now_s)
{
	heard_expire(l, now_s);
	uint8_t i;
	for (i = 0; i < l->n; i++) {
		const heard_t *e = &l->e[i];
		if (e->kind == k->kind && e->id == k->id && e->tg == k->tg)
			break;
	}
	heard_t e;
	if (i < l->n) {
		e = l->e[i];
		if ((new_call || heard_age_s(&e, now_s) >= HEARD_GAP_S) && e.count < 255u)
			e.count++;
	} else {
		e = *k;
		e.count = 1;
		if (l->n < HEARD_MAX)
			l->n++;
		i = (uint8_t)(l->n - 1u);	/* full: the oldest (the last) makes room */
	}
	e.hz = k->hz != 0 ? k->hz : e.hz;
	e.nac = k->nac;
	e.last_s = now_s & HEARD_CLOCK_MASK;
	memmove(&l->e[1], &l->e[0], i * sizeof l->e[0]);
	l->e[0] = e;
}

void heard_pack_call(const char *call, heard_t *e)
{
	uint64_t v = 0;
	for (unsigned c = 0; c < HEARD_CALL_CHARS; c++) {
		char ch = *call != '\0' ? *call++ : ' ';
		if (ch >= 'a' && ch <= 'z')
			ch = (char)(ch - 'a' + 'A');
		const char *at = strchr(call_set, ch);
		unsigned x = at != NULL && ch != '\0' ? (unsigned)(at - call_set) : sizeof call_set - 2u;
		v = v << 6 | x;
	}
	e->id = (uint32_t)(v >> 24) & 0xFFFFFFu;
	e->tg = (uint32_t)v & 0xFFFFFFu;
}

void heard_unpack_call(const heard_t *e, char out[HEARD_CALL_CHARS + 1])
{
	uint64_t v = (uint64_t)e->id << 24 | e->tg;
	for (unsigned c = HEARD_CALL_CHARS; c-- > 0;) {
		unsigned x = (unsigned)(v & 63u);
		out[c] = x < sizeof call_set - 1u ? call_set[x] : '?';
		v >>= 6;
	}
	unsigned n = HEARD_CALL_CHARS;
	while (n > 0 && out[n - 1u] == ' ')
		n--;
	out[n] = '\0';
}
#endif
