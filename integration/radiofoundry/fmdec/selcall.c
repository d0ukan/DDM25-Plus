/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DTMF / SelCall decoder (selcall.h describes the method). Written for this project. */
#include "config.h"
#if CONFIG_SELCALL
#include <string.h>
#include "selcall.h"

#define SETS		4u
#define FRAME_PURE_DIV	8u	/* a frame's periods spread (sd) under 1/8 of their mean ... */
#define FRAME_SLACK	51u	/* ... plus 0.2 sample (1/256 sample) */
#define SEG_TOL_PCT	3u	/* a frame within 3 % of the segment's frequency belongs to it */
#define SEG_MIN_FRAMES	4u	/* 20 ms */
#define MERGE_PERMILLE	20u	/* the same tone again within 2 % ... */
#define MERGE_GAP	80u	/* ... and 10 ms (samples) */
#define SEQ_END		1200u	/* 150 ms without a tone ends a sequence (samples) */
#define LOST_GAP	160u	/* 20 ms between two tones: one was lost (samples; the shortest, EIA, is 33 ms) */
#define SEQ_MIN		4u	/* tones in a sequence */
#define FIT_PERMILLE	20u	/* a tone within 2 % of the set's */
#define TRIM_PERMILLE	50u	/* a tone at an end 5 % from every set's tone is no part of the call */
#define MIN_LEVEL	16	/* crossings are counted above this peak (counts) */
#define PERIOD_MAX	(255u * 256u)	/* 32 ms: longer is a gap, not a period */
#ifndef PAIR_SHARE_PCT
#define PAIR_SHARE_PCT	60	/* a DTMF pair holds at least 60 % of its block's power */
#endif
#define LETTER_COST	10u	/* a tone read as A-D or F costs as much as 1 % of frequency error */

static const char digits[] = "0123456789ABCDEF";

/* Hz, by character 0-9 A-F; 'E' (14) is the repeat tone */
static const uint16_t tones[SETS][16] = {
	{ 2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200, 2800, 810, 970, 885, 2600, 680 },	/* ZVEI1 */
	{ 2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200, 885, 825, 740, 680, 970, 2600 },	/* ZVEI2 */
	{ 1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860, 2400, 930, 2247, 991, 2110, 1055 },	/* CCIR */
	{ 600, 741, 882, 1023, 1164, 1305, 1446, 1587, 1728, 1869, 2151, 2433, 2010, 2292, 459, 1091 },	/* EIA */
};
static const uint8_t tone_ms[SETS] = { 70, 70, 100, 33 };
/* A tone inside a call lasts this much of the set's length, in % (measured with the frame a change of tone costs
 * it added back). ZVEI's 70 ms is 70 % of CCIR's 100 ms and CCIR's 143 % of ZVEI's: their calls are told apart by
 * length where their tones are within 2 % (ZVEI 3 1270 Hz, CCIR 3 1275 Hz). EIA's 33 ms tones are measured to the
 * 5 ms frame: a wider window, still far from ZVEI's 227 %. The first and last tone: at least the minimum */
static const uint8_t len_min[SETS] = { 80, 80, 80, 60 }, len_max[SETS] = { 130, 130, 130, 160 };
#define REPEAT	14u

const char *selcall_set_name(uint8_t set)
{
	static const char *const names[] = { "SelCall", "ZVEI1", "ZVEI2", "CCIR", "EIA", "ZVEI", "DTMF" };
	return set <= SELCALL_DTMF ? names[set] : names[0];
}

uint16_t selcall_tone_hz(uint8_t set, char c)
{
	const char *p = c != '\0' ? strchr(digits, c) : NULL;
	if (set < SELCALL_ZVEI1 || set > SELCALL_EIA || p == NULL)
		return 0;
	return tones[set - 1u][p - digits];
}

uint16_t selcall_tone_ms(uint8_t set)
{
	return set >= SELCALL_ZVEI1 && set <= SELCALL_EIA ? tone_ms[set - 1u] : 0;
}

static void emit(selcall_t *d, uint8_t set, const char *text)
{
	selcall_seq_t s;
	s.set = set;
	size_t n = strlen(text);
	if (n > SELCALL_TEXT)
		n = SELCALL_TEXT;
	memcpy(s.text, text, n);
	s.text[n] = '\0';
	if (d->fn != NULL)
		d->fn(d->ctx, &s);
}

/* --- DTMF (FM caller ID's decoder) --- */

/* 2 cos(2 pi f / 8000), Q14: the 8 DTMF tones (697 770 852 941, 1209 1336 1477 1633 Hz) */
static const int16_t dtmf_c1[8] = { 27980, 26956, 25701, 24219, 19073, 16325, 13085, 9315 };
static const char dtmf_keys[] = "123A456B789C*0#D";

/* Goertzel power of the block in hist (oldest at hpos) at one coefficient, scaled down */
static int64_t power(const selcall_t *d, int32_t c)
{
	int64_t s1 = 0, s2 = 0;
	for (uint16_t i = 0; i < FMID_DTMF_N; i++) {
		uint16_t k = (uint16_t)(d->hpos + i);
		int64_t s = d->hist[k >= FMID_DTMF_N ? k - FMID_DTMF_N : k] + ((c * s1) >> 14) - s2;
		s2 = s1;
		s1 = s;
	}
	s1 >>= 8;
	s2 >>= 8;
	return s1 * s1 + s2 * s2 - (((c * s1) >> 14) * s2);
}

/* A digit FM caller ID's decoder took: real DTMF is two pure tones with little else (a sender 1.5 % off still
 * leaves its pair 70 % and more of the power), voice that happens to put two harmonics on a DTMF pair (a vowel's
 * formants near 700 and 1200 Hz) more besides. The digit counts only when the pair holds at least PAIR_SHARE_PCT
 * of the block it ended in (FM caller ID asks 45 %): a tone's share is 2 |X|^2 / (N E), |X|^2 scaled down 2^16 */
static bool dtmf_pure(const selcall_t *d, char digit)
{
	const char *p = strchr(dtmf_keys, digit);
	if (p == NULL || digit == '\0')
		return false;
	int r = (int)(p - dtmf_keys) / 4, c = 4 + (int)(p - dtmf_keys) % 4;
	int64_t base = power(d, dtmf_c1[r]) + power(d, dtmf_c1[c]);
	int64_t e = 0;
	for (uint16_t i = 0; i < FMID_DTMF_N; i++)
		e += (int64_t)d->hist[i] * d->hist[i];
	return base * 2 * 65536 * 100 >= (int64_t)PAIR_SHARE_PCT * FMID_DTMF_N * e;
}

static void on_fmid(void *ctx, const fmid_event_t *e)
{
	selcall_t *d = ctx;
	if (e->kind != FMID_DTMF)
		return;
	if (!dtmf_pure(d, e->digit)) {
		d->dtmf_refused++;
		return;
	}
	size_t n = strlen(d->digits);
	if (n < SELCALL_TEXT) {
		d->digits[n] = e->digit;
		d->digits[n + 1] = '\0';
	}
	d->digit_at = d->n;
}

static void dtmf_end(selcall_t *d)
{
	if (d->digits[0] == '\0')
		return;
	emit(d, SELCALL_DTMF, d->digits);
	d->digits[0] = '\0';
}

/* --- sequences --- */

/* The text a set reads n tones from first as, and its error (0 when they don't fit) */
static uint32_t fit(const selcall_t *d, uint8_t set, uint8_t first, uint8_t n, char *text)
{
	uint32_t cost = 1;
	size_t len = 0;
	uint16_t len_ms = tone_ms[set];
	for (uint8_t i = 0; i < n; i++) {
		const selcall_seg_t *s = &d->seg[first + i];
		uint32_t best = 0xFFFFFFFFu;
		uint8_t bj = 0;
		for (uint8_t j = 0; j < 16u; j++) {
			uint32_t want = tones[set][j] * 4u;
			uint32_t err = s->f4 > want ? s->f4 - want : want - s->f4;
			uint32_t pm = err * 1000u / want;
			if (pm < best) {
				best = pm;
				bj = j;
			}
		}
		uint32_t want = tones[set][bj] * 4u;
		if ((s->f4 > want ? s->f4 - want : want - s->f4) * 1000u > want * FIT_PERMILLE)
			return 0;	/* (exactly: the per mille above is rounded down) */
		cost += best;
		/* its length in % of the set's, with the frame a change of tone costs it (5 ms) back */
		uint32_t q = ((uint32_t)s->ms + SELCALL_FRAME * 1000u / SELCALL_FS) * 100u / len_ms;
		if (q < len_min[set] || (i > 0 && i + 1u < n && q > len_max[set]))
			return 0;
		if (i > 0 && i + 1u < n)
			cost += (q > 100u ? q - 100u : 100u - q) / 2u;
		if (bj >= 10u && bj != REPEAT)
			cost += LETTER_COST;	/* sequences are digits: a letter is the less likely reading */
		if (len < SELCALL_TEXT) {
			char c = digits[bj];
			if (bj == REPEAT && len > 0)
				c = text[len - 1];
			text[len++] = c;
		}
	}
	text[len] = '\0';
	return cost;
}

/* Whether a frequency is within 5 % of a tone of any set (the one picked or not: another set's call is never cut
 * down to a part that fits the one picked). A tone at a call's end is left out only when it is not: a whistle
 * merged with the call's first or last tone (they are less than 3 % apart, or the segments would have split)
 * averages to within 5 % of that tone, and the call is then refused rather than shown a digit short */
static bool any_tone(uint16_t f4)
{
	for (uint8_t s = 0; s < SETS; s++) {
		for (uint8_t j = 0; j < 16u; j++) {
			uint32_t want = tones[s][j] * 4u;
			uint32_t err = f4 > want ? f4 - want : want - f4;
			if (err * 1000u <= want * TRIM_PERMILLE)
				return true;
		}
	}
	return false;
}

static void seq_end(selcall_t *d)
{
	/* a tone too short to count right after the last one: a tone was lost there */
	if (d->nseg > 0 && d->short_end > d->seg[d->nseg - 1u].end &&
	    d->short_end - d->seg[d->nseg - 1u].end <= LOST_GAP)
		d->broken = true;
	if (d->broken && d->nseg >= SEQ_MIN) {
		d->seqs_rejected++;
	} else if (d->nseg >= SEQ_MIN) {
		/* tones at the ends that are no set's tone at all are left out (a whistle or a vowel heard as a tone right
		 * before or after the call, with no pause between); a stretch inside is never picked out on its own: part of
		 * another set's call could fit */
		uint8_t first = 0, n = d->nseg;
		while (n > 0 && !any_tone(d->seg[first].f4)) {
			first++;
			n--;
		}
		while (n > 0 && !any_tone(d->seg[first + n - 1u].f4))
			n--;
		char text[SETS][SELCALL_TEXT + 1];
		uint32_t cost[SETS];
		uint8_t best = 0xFF;
		for (uint8_t s = 0; s < SETS && n >= SEQ_MIN; s++) {
			bool on = d->only == SELCALL_ANY || d->only == s + 1u;
			cost[s] = on ? fit(d, s, first, n, text[s]) : 0;
			if (cost[s] != 0 && (best == 0xFF || cost[s] < cost[best]))
				best = s;
		}
		if (best == 0xFF) {
			d->seqs_rejected++;
		} else {
			uint8_t set = (uint8_t)(best + 1u);
			/* ZVEI1 and ZVEI2 read alike (digits only): just "ZVEI" */
			if (best <= 1u && cost[0] != 0 && cost[1] != 0 && strcmp(text[0], text[1]) == 0)
				set = SELCALL_ZVEI;
			emit(d, set, text[best]);
		}
	}
	d->nseg = 0;
	d->broken = false;
}

static void seg_close(selcall_t *d)
{
	if (d->s_cnt == 0)
		return;
	uint16_t frames = d->s_frames;
	uint32_t sum = d->s_sum, cnt = d->s_cnt, start = d->s_start, end = d->s_end;
	d->s_cnt = 0;
	d->s_sum = 0;
	d->s_frames = 0;
	if (frames < SEG_MIN_FRAMES || sum == 0) {
		/* a tone too short to count, in the sets' range (450-2900 Hz; noise's slow crossings are not): one may have
		 * been lost here */
		uint32_t hz = sum != 0 ? (uint32_t)((uint64_t)SELCALL_FS * 256u * cnt / sum) : 0;
		if (frames >= 2u && hz >= 450u && hz <= 2900u)
			d->short_end = end;
		return;
	}
	d->segs_seen++;
	uint16_t f4 = (uint16_t)(((uint64_t)SELCALL_FS * 4u * 256u * cnt + sum / 2u) / sum);
	uint16_t ms = (uint16_t)(frames * (SELCALL_FRAME * 1000u / SELCALL_FS));
	if (d->nseg > 0) {
		selcall_seg_t *p = &d->seg[d->nseg - 1u];
		uint32_t err = f4 > p->f4 ? f4 - p->f4 : p->f4 - f4;
		if (err * 1000u <= (uint32_t)p->f4 * MERGE_PERMILLE && start - p->end <= MERGE_GAP) {
			p->ms = (uint16_t)(p->ms + ms + (start - p->end) * 1000u / SELCALL_FS);
			p->end = end;
			return;
		}
	}
	if (d->nseg >= SELCALL_SEGS)
		seq_end(d);	/* too long for one: what there is, then a new one */
	/* SelCall tones follow each other with no gap: more than LOST_GAP between two, or a short tone just before the
	 * first, is a tone lost (noise): the sequence would read short or shifted, so it is refused */
	if (d->nseg > 0 && start - d->seg[d->nseg - 1u].end > LOST_GAP)
		d->broken = true;
	if (d->nseg == 0 && d->short_end != 0 && start - d->short_end <= LOST_GAP)
		d->broken = true;
	selcall_seg_t *s = &d->seg[d->nseg++];
	s->f4 = f4;
	s->ms = ms;
	s->end = end;
}

/* Two mean periods within SEG_TOL_PCT of the second */
static bool near_pct(uint32_t a, uint32_t b)
{
	return (a > b ? a - b : b - a) * 100u <= b * SEG_TOL_PCT;
}

static void frame_end(selcall_t *d)
{
	uint32_t cnt = d->f_cnt, sum = d->f_sum;
	/* The periods' spread (standard deviation) is under 1/FRAME_PURE_DIV of their mean plus FRAME_SLACK: a crossing
	 * put between two samples by a straight line is up to 0.06 sample off at 2.4 kHz (3.3 samples a period), and noise
	 * moves it more there; voice and noise spread 30-50 %. Their mean is exact whatever the spread: the periods add up
	 * to the time from the first crossing to the last */
	bool pure = false;
	if (cnt > 0 && d->env >= (MIN_LEVEL << 4)) {
		uint64_t mean = sum / cnt;
		uint64_t var = d->f_sq / cnt - mean * mean;	/* (1/256 sample)^2 */
		uint64_t lim = mean / FRAME_PURE_DIV;
		pure = var <= lim * lim + (uint64_t)FRAME_SLACK * FRAME_SLACK;
	}
	d->f_cnt = 0;
	d->f_sum = 0;
	d->f_sq = 0;
	if (!pure) {
		d->o_cnt = 0;	/* a frame off the tone just before: the change of tone */
		seg_close(d);
	} else {
		uint32_t fm = sum / cnt;
		bool off = d->s_cnt > 0 && !near_pct(fm, d->s_sum / d->s_cnt);
		if (off && d->o_cnt == 0) {
			/* one frame off the tone: kept aside (noise moves a 459 Hz frame's two periods that far now and then);
			 * the tone goes on if the next frame is back on it */
			d->o_sum = sum;
			d->o_cnt = (uint16_t)cnt;
			goto done;
		}
		if (off) {
			/* two frames off in a row: another tone, from the first of them if they agree */
			bool pair = near_pct(fm, d->o_sum / d->o_cnt);
			uint32_t o_sum = d->o_sum;
			uint16_t o_cnt = d->o_cnt;
			d->o_cnt = 0;
			d->s_end = d->n - 2u * SELCALL_FRAME;
			seg_close(d);
			if (pair) {
				d->s_start = d->n - 2u * SELCALL_FRAME;
				d->s_sum = o_sum;
				d->s_cnt = o_cnt;
				d->s_frames = 1;
			}
		}
		if (d->o_cnt != 0) {
			/* back on the tone after one frame off: that frame was the tone too */
			d->s_sum += d->o_sum;
			d->s_cnt = (uint16_t)(d->s_cnt + d->o_cnt);
			d->s_frames++;
			d->o_cnt = 0;
		}
		if (d->s_cnt == 0)
			d->s_start = d->n - SELCALL_FRAME;
		d->s_sum += sum;
		d->s_cnt = (uint16_t)(d->s_cnt + cnt);
		d->s_frames++;
		d->s_end = d->n;
	}
done:
	if (d->nseg > 0 && d->s_cnt == 0 && d->n - d->seg[d->nseg - 1u].end > SEQ_END)
		seq_end(d);
	if (d->digits[0] != '\0' && d->n - d->digit_at > SELCALL_DTMF_END * (SELCALL_FS / 1000u))
		dtmf_end(d);
}

static void sample(selcall_t *d, int32_t x)
{
	d->hp += ((x * 16) - d->hp) >> 3;	/* about 160 Hz */
	int32_t y = x - (d->hp >> 4);
	int32_t a = (y < 0 ? -y : y) * 16;
	if (a > d->env)
		d->env = a;
	else
		d->env -= d->env >> 7;
	int32_t th = d->env >> 6;	/* a quarter of the peak */
	if (th < MIN_LEVEL)
		th = MIN_LEVEL;
	if (y < -th)
		d->armed = true;
	if (d->armed && d->prev <= 0 && y > 0) {
		d->armed = false;
		uint32_t frac = (uint32_t)((int64_t)(-d->prev) * 256 / (y - d->prev));
		uint32_t t = ((d->n - 1u) << 8) + frac;
		if (d->have_x) {
			uint32_t p = t - d->last_x;
			if (p <= PERIOD_MAX) {
				d->f_cnt++;
				d->f_sum += p;
				d->f_sq += (uint64_t)p * p;
			}
		}
		d->last_x = t;
		d->have_x = true;
	}
	d->prev = y;
	d->n++;
	if (++d->f_pos >= SELCALL_FRAME) {
		d->f_pos = 0;
		frame_end(d);
	}
}

void selcall_init(selcall_t *d, uint8_t only, selcall_fn fn, void *ctx)
{
	memset(d, 0, sizeof *d);
	fmid_init(&d->dtmf, on_fmid, d);
	d->only = only;
	d->fn = fn;
	d->ctx = ctx;
}

void selcall_only(selcall_t *d, uint8_t only)
{
	d->only = only;
}

void selcall_audio(selcall_t *d, const int16_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		/* sample by sample, so that the last FMID_DTMF_N kept are FM caller ID's DTMF block when it takes a digit */
		d->hist[d->hpos] = s[i];
		d->hpos = (uint8_t)(d->hpos + 1u >= FMID_DTMF_N ? 0u : d->hpos + 1u);
		fmid_audio(&d->dtmf, &s[i], 1);
		sample(d, s[i]);
	}
}

void selcall_flush(selcall_t *d)
{
	seg_close(d);
	seq_end(d);
	dtmf_end(d);
}
#endif
