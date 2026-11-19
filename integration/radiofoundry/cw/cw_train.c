/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus CW practice (cw_train.h). No hardware, no RTOS: the hand-over is a sequence lock. */
#include "config.h"
#if CONFIG_CW_RX
#include <string.h>
#include "radio/cw_train.h"
#include "radio/morse.h"

#define LOAD(x)		__atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

const char cw_koch[CW_TRAIN_KOCH_N + 1u] = "KMURESNAPTLWI.JZ=FOY,VG5/Q92H38B?47C1D60X";

static uint32_t next(uint32_t *rng)
{
	uint32_t x = *rng != 0 ? *rng : 0x9E3779B9u;	/* xorshift32 */
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*rng = x;
	return x;
}

char cw_train_char(uint32_t *rng, uint8_t level)
{
	if (level < CW_TRAIN_LEVEL_MIN)
		level = CW_TRAIN_LEVEL_MIN;
	if (level > CW_TRAIN_KOCH_N)
		level = CW_TRAIN_KOCH_N;
	uint32_t r = next(rng);
	if (level > CW_TRAIN_LEVEL_MIN && r % 3u == 0)
		return cw_koch[level - 1u];	/* the newest character more often */
	return cw_koch[(r >> 8) % level];
}

void cw_train_group(uint32_t *rng, uint8_t level, uint8_t n, char *out)
{
	if (n > CW_TRAIN_MAX)
		n = CW_TRAIN_MAX;
	for (uint8_t i = 0; i < n; i++)
		out[i] = cw_train_char(rng, level);
}

uint8_t cw_train_right(const char *sent, uint8_t n, const char *ans, uint8_t m)
{
	uint8_t row[CW_TRAIN_MAX + 1u], prev[CW_TRAIN_MAX + 1u];
	if (n > CW_TRAIN_MAX)
		n = CW_TRAIN_MAX;
	if (m > CW_TRAIN_MAX)
		m = CW_TRAIN_MAX;
	for (uint8_t j = 0; j <= m; j++)
		prev[j] = j;
	for (uint8_t i = 1; i <= n; i++) {
		row[0] = i;
		for (uint8_t j = 1; j <= m; j++) {
			uint8_t sub = (uint8_t)(prev[j - 1] + (sent[i - 1] != ans[j - 1] ? 1u : 0u));
			uint8_t del = (uint8_t)(prev[j] + 1u), ins = (uint8_t)(row[j - 1] + 1u);
			row[j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
		}
		memcpy(prev, row, (size_t)m + 1u);
	}
	return prev[m] >= n ? 0 : (uint8_t)(n - prev[m]);
}

/* A character's elements, dashes counted */
static uint8_t elements(char c, uint8_t *dashes)
{
	uint8_t code = morse_code(c), n = 0;
	*dashes = 0;
	if (code == 0)
		return 0;
	uint8_t marker = 0x80;
	while ((code & marker) == 0)
		marker >>= 1;
	for (uint8_t b = (uint8_t)(marker >> 1); b != 0; b >>= 1) {
		n++;
		*dashes = (uint8_t)(*dashes + ((code & b) != 0 ? 1u : 0u));
	}
	return n;
}

uint32_t cw_train_ms(const char *text, uint8_t len, uint8_t wpm)
{
	uint32_t units = 0;
	bool any = false, space = false;
	for (uint8_t i = 0; i < len; i++) {
		uint8_t dashes, n = elements(text[i], &dashes);
		if (text[i] == ' ') {
			space = true;
			continue;
		}
		if (n == 0)
			continue;
		if (any)
			units += space ? 7u : 3u;
		units += n + 2u * dashes + (n - 1u);	/* the elements and the gaps inside */
		any = true;
		space = false;
	}
	return units * morse_dot_ms(wpm);
}

void cw_train_pattern(char c, char *out)
{
	uint8_t code = morse_code(c), k = 0;
	if (code != 0) {
		uint8_t marker = 0x80;
		while ((code & marker) == 0)
			marker >>= 1;
		for (uint8_t b = (uint8_t)(marker >> 1); b != 0; b >>= 1)
			out[k++] = (code & b) != 0 ? '-' : '.';
	}
	out[k] = '\0';
}

/* --- the hand-over: odd while the UI writes --- */
static cw_play_t req;
static uint32_t req_seq, taken_seq;
static uint8_t state;

void cw_train_play(const char *text, uint8_t len, uint8_t wpm, uint16_t hz)
{
	uint32_t s = LOAD(req_seq);
	STORE(req_seq, s + 1u);
	if (len > CW_TRAIN_MAX)
		len = CW_TRAIN_MAX;
	memcpy(req.text, text, len);
	req.len = len;
	req.wpm = wpm;
	req.hz = hz;
	STORE(state, (uint8_t)CW_PLAY_PENDING);
	STORE(req_seq, s + 2u);
}

uint8_t cw_train_state(void)
{
	return LOAD(state);
}

bool cw_train_take(cw_play_t *out)
{
	uint32_t s = LOAD(req_seq);
	if (s == taken_seq || (s & 1u) != 0)
		return false;
	*out = req;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if (LOAD(req_seq) != s)
		return false;	/* rewritten meanwhile: the next poll takes the new one */
	taken_seq = s;
	return true;
}

void cw_train_done(bool played)
{
	if (LOAD(req_seq) == taken_seq)	/* not if a newer one is pending already */
		STORE(state, (uint8_t)(played ? CW_PLAY_PLAYING : CW_PLAY_REFUSED));
}
#endif
