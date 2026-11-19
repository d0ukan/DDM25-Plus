/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus QSO log (qso_log.h): the marks and frequencies beside Recent, the automatic mark, the export. Portable (no
 * RTOS): the host tests run it against their own Recent list. */
#include "config.h"
#if CONFIG_QSO_LOG
#include <string.h>
#include "ui/qso_log.h"
#include "ui/ui_nav.h"

static volatile uint32_t seq;		/* odd while Recent changes (the export retries) */
static uint32_t mark, user, fresh;	/* bit i: entry i is a QSO / the user chose / new since the last end */
static uint32_t hz[UI_RECENT_MAX];
static uint32_t tx_ms, tx_hz;		/* our last transmission seen and its channel (RX frequency); tx_ms 0: none */

_Static_assert(UI_RECENT_MAX <= 32, "a bit an entry");

static void barrier(void) { __asm__ volatile("" ::: "memory"); }

void qso_log_begin(void)
{
	seq++;
	barrier();
}

void qso_log_reset(void)
{
	qso_log_begin();
	mark = user = fresh = 0;
	memset(hz, 0, sizeof hz);
	tx_ms = tx_hz = 0;
	barrier();
	seq++;
}

static uint32_t shifted(uint32_t bits, uint8_t i, bool carry)
{
	uint32_t low = (1u << i) - 1u, above = ~((2u << i) - 1u);
	uint32_t moved = carry ? (bits >> i) & 1u : 0u;
	return (bits & above) | ((bits & low) << 1) | moved;
}

void qso_log_shift(uint8_t i, bool dup)
{
	if (i >= UI_RECENT_MAX)
		return;
	mark = shifted(mark, i, dup);
	user = shifted(user, i, dup);
	fresh = shifted(fresh, i, false) | 1u;
	memmove(&hz[1], &hz[0], i * sizeof hz[0]);
	hz[0] = 0;
}

static uint32_t span(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

void qso_log_end(const radio_view_t *v, uint32_t now_ms)
{
	uint8_t n = ui_recent_count();
	for (uint8_t k = 0; k < n; k++) {
		if (fresh & (1u << k))
			hz[k] = v->ch.rx_hz;	/* the channel it was heard (or made) on */
	}
	fresh = 0;
	if (v->state == RS_TX) {
		tx_ms = now_ms == 0 ? 1u : now_ms;
		tx_hz = v->ch.rx_hz;
	}
	/* The automatic mark: a heard call with our TX on its frequency within QSO_AUTO_MS (a DMR call we made is an entry
	 * of its own, any other TX is the one last seen) */
	for (uint8_t k = 0; k < n; k++) {
		const ui_recent_t *r = ui_recent_get(k);
		uint32_t bit = 1u << k;
		if (r == NULL || r->out || (user & bit) || (mark & bit) || hz[k] == 0)
			continue;
		bool qso = tx_ms != 0 && tx_hz == hz[k] && span(r->ms, tx_ms) <= QSO_AUTO_MS;
		for (uint8_t j = 0; j < n && !qso; j++) {
			const ui_recent_t *o = ui_recent_get(j);
			qso = o != NULL && o->out && hz[j] == hz[k] && span(r->ms, o->ms) <= QSO_AUTO_MS;
		}
		if (qso)
			mark |= bit;
	}
	barrier();
	seq++;
}

bool qso_log_marked(uint8_t i)
{
	return i < UI_RECENT_MAX && (mark >> i) & 1u;
}

bool qso_log_user(uint8_t i)
{
	return i < UI_RECENT_MAX && (user >> i) & 1u;
}

uint32_t qso_log_hz(uint8_t i)
{
	return i < UI_RECENT_MAX ? hz[i] : 0u;
}

bool qso_log_toggle(uint8_t i)
{
	if (i >= UI_RECENT_MAX || i >= ui_recent_count())
		return false;
	qso_log_begin();
	mark ^= 1u << i;
	user |= 1u << i;
	barrier();
	seq++;
	return qso_log_marked(i);
}

static void put16(uint8_t *p, uint32_t x)
{
	p[0] = (uint8_t)x;
	p[1] = (uint8_t)(x >> 8);
}

static void put32(uint8_t *p, uint32_t x)
{
	put16(p, x);
	put16(p + 2, x >> 16);
}

size_t qso_log_export(uint8_t *out, size_t cap, uint32_t now_ms)
{
	uint32_t s = seq;
	barrier();
	if (s & 1u)
		return 0;
	uint8_t n = ui_recent_count();
	if (n > UI_RECENT_MAX)
		n = UI_RECENT_MAX;
	size_t len = QSO_EXPORT_HEAD + (size_t)n * QSO_EXPORT_ENTRY;
	if (cap < len)
		return 0;
	out[0] = QSO_EXPORT_VERSION;
	out[1] = n;
	put32(out + 2, now_ms);
	put32(out + 6, QSO_AUTO_MS / 1000u);
	for (uint8_t k = 0; k < n; k++) {
		const ui_recent_t *r = ui_recent_get(k);
		uint8_t *e = out + QSO_EXPORT_HEAD + (size_t)k * QSO_EXPORT_ENTRY;
		memset(e, 0, QSO_EXPORT_ENTRY);
		if (r == NULL)
			return 0;
		uint8_t f = (r->out ? QSO_F_OUT : 0) | (r->group ? QSO_F_GROUP : 0) | (r->to_us ? QSO_F_TO_US : 0) |
			    (((mark >> k) & 1u) ? QSO_F_QSO : 0) | (((user >> k) & 1u) ? QSO_F_USER : 0);
		uint8_t mode = 0;
		uint16_t nac = 0;
#if CONFIG_CALL_LOG
		mode = r->mode;
		nac = r->nac;
		f |= r->enc ? QSO_F_ENC : 0;
#endif
#if CONFIG_P25_EXTRAS
		f |= r->emerg ? QSO_F_EMERG : 0;
#endif
		put32(e, now_ms - r->ms);
		e[4] = mode;
		e[5] = f;
		put16(e + 6, nac);
		put32(e + 8, r->src);
		put32(e + 12, r->dst);
		put32(e + 16, hz[k]);
		for (unsigned c = 0; c < QSO_NAME_BYTES && r->name[c] != '\0'; c++)
			e[20 + c] = (uint8_t)r->name[c];
	}
	barrier();
	return seq == s ? len : 0;
}
#endif
