/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus CW decoder (cwdec.h). Integer only; the 64-bit arithmetic runs once a block. */
#include <string.h>
#include "radio/cwdec.h"

#define RATIO_TONE	12u	/* the peak bin over the far bins' mean: a tone */
#define FAR_BINS	4u	/* bins this far from the peak (100 Hz) make the floor */
#define LONG_UNITS	16u	/* a mark longer than this is a held tone, not Morse */
#define SPACE_MAX_MS	60000u	/* spaces are measured up to this (no overflow after hours) */
#define PAUSE_UNITS	14u	/* a space this long (two word spaces): what follows is learned afresh */
#define DOT16_MIN	(16u * 25u)	/* 48 WPM */
#define DOT16_MAX	(16u * 300u)	/* 4 WPM */

/* 2 cos(2 pi f / 8000), Q12, f = 400, 425 .. 1000 Hz */
static const int16_t coef[CWDEC_BINS] = {
	7791, 7740, 7686, 7629, 7568, 7505, 7440, 7371, 7299, 7225, 7147, 7068, 6985,
	6899, 6811, 6721, 6627, 6532, 6433, 6333, 6229, 6124, 6016, 5905, 5793,
};

/* One period of a sine, Q15 */
static const int16_t sine[64] = {
	0, 3212, 6393, 9512, 12539, 15446, 18204, 20787,
	23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609,
	32767, 32609, 32137, 31356, 30273, 28898, 27245, 25329,
	23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212,
	0, -3212, -6393, -9512, -12539, -15446, -18204, -20787,
	-23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609,
	-32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329,
	-23170, -20787, -18204, -15446, -12539, -9512, -6393, -3212,
};

/* ITU-R M.1677-1 and the common prosigns, by code (elements MSB first after a leading 1; 0 dot, 1 dash) */
static const struct {
	uint16_t code;
	char text[6];
} table[] = {
	{ 0x002, "E" },	/* . */
	{ 0x003, "T" },	/* - */
	{ 0x004, "I" },	/* .. */
	{ 0x005, "A" },	/* .- */
	{ 0x006, "N" },	/* -. */
	{ 0x007, "M" },	/* -- */
	{ 0x008, "S" },	/* ... */
	{ 0x009, "U" },	/* ..- */
	{ 0x00A, "R" },	/* .-. */
	{ 0x00B, "W" },	/* .-- */
	{ 0x00C, "D" },	/* -.. */
	{ 0x00D, "K" },	/* -.- */
	{ 0x00E, "G" },	/* --. */
	{ 0x00F, "O" },	/* --- */
	{ 0x010, "H" },	/* .... */
	{ 0x011, "V" },	/* ...- */
	{ 0x012, "F" },	/* ..-. */
	{ 0x014, "L" },	/* .-.. */
	{ 0x016, "P" },	/* .--. */
	{ 0x017, "J" },	/* .--- */
	{ 0x018, "B" },	/* -... */
	{ 0x019, "X" },	/* -..- */
	{ 0x01A, "C" },	/* -.-. */
	{ 0x01B, "Y" },	/* -.-- */
	{ 0x01C, "Z" },	/* --.. */
	{ 0x01D, "Q" },	/* --.- */
	{ 0x020, "5" },	/* ..... */
	{ 0x021, "4" },	/* ....- */
	{ 0x022, "<VE>" },	/* ...-. understood (SN) */
	{ 0x023, "3" },	/* ...-- */
	{ 0x027, "2" },	/* ..--- */
	{ 0x028, "<AS>" },	/* .-... wait */
	{ 0x02A, "<AR>" },	/* .-.-. end of message (ITU "+") */
	{ 0x02F, "1" },	/* .---- */
	{ 0x030, "6" },	/* -.... */
	{ 0x031, "=" },	/* -...- BT */
	{ 0x032, "/" },	/* -..-. */
	{ 0x035, "<KA>" },	/* -.-.- start */
	{ 0x036, "<KN>" },	/* -.--. go ahead, named station (ITU "(") */
	{ 0x038, "7" },	/* --... */
	{ 0x03C, "8" },	/* ---.. */
	{ 0x03E, "9" },	/* ----. */
	{ 0x03F, "0" },	/* ----- */
	{ 0x045, "<SK>" },	/* ...-.- end of contact */
	{ 0x04C, "?" },	/* ..--.. */
	{ 0x04D, "_" },	/* ..--.- */
	{ 0x052, "\"" },	/* .-..-. */
	{ 0x055, "." },	/* .-.-.- */
	{ 0x05A, "@" },	/* .--.-. */
	{ 0x05E, "'" },	/* .----. */
	{ 0x061, "-" },	/* -....- */
	{ 0x06A, ";" },	/* -.-.-. */
	{ 0x06B, "!" },	/* -.-.-- */
	{ 0x06D, ")" },	/* -.--.- */
	{ 0x073, "," },	/* --..-- */
	{ 0x078, ":" },	/* ---... */
	{ 0x089, "$" },	/* ...-..- */
	{ 0x0C5, "<BK>" },	/* -...-.- break */
	{ 0x1A4, "<CL>" },	/* -.-..-.. closing */
	{ 0x238, "<SOS>" },	/* ...---... */
};

const char *cwdec_lookup(uint16_t code)
{
	for (unsigned i = 0; i < sizeof table / sizeof table[0]; i++)
		if (table[i].code == code)
			return table[i].text;
	for (unsigned n = 7; n <= 12; n++)
		if (code == (uint16_t)(1u << n))
			return "<HH>";	/* 7 dots or more: an error, the word is sent again */
	return NULL;
}

static int32_t sin_q15(uint32_t ph)
{
	uint32_t idx = ph >> 26;
	int32_t frac = (int32_t)((ph >> 16) & 0x3FFu);
	int32_t a = sine[idx], b = sine[(idx + 1u) & 63u];
	return a + (((b - a) * frac) >> 10);
}

static uint32_t isqrt(uint32_t v)
{
	uint32_t r = 0, bit = 1u << 30;
	while (bit > v)
		bit >>= 2;
	while (bit != 0) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

void cwdec_init(cwdec_t *d, cwdec_text_fn out, void *ctx)
{
	memset(d, 0, sizeof *d);
	d->out = out;
	d->ctx = ctx;
	d->kh[0] = d->kh[1] = d->kh[2] = 0xFFu;
	d->track = 0xFFu;
	d->dot16 = 16u * 60u;		/* 20 WPM until marks say otherwise */
	d->dash16 = 16u * 180u;
	d->code = 1;
	d->spaced = true;
	d->learn = true;
}

/* --- timing --- */

static uint32_t unit16(const cwdec_t *d)
{
	return ((uint32_t)d->dash16 - d->dot16) / 2u;	/* the ratio is kept >= 2: at least half a dot */
}

/* The keyer's weight (and the detector's): how much longer each mark is than its units, ms x 16 */
static int32_t weight16(const cwdec_t *d)
{
	return (int32_t)d->dot16 - (int32_t)unit16(d);
}

static void emit(cwdec_t *d, const char *s)
{
	if (d->out != NULL)
		d->out(d->ctx, s);
}

static void end_char(cwdec_t *d)
{
	if (d->code > 1u) {
		const char *s = d->code == 0xFFFFu ? NULL : cwdec_lookup(d->code);
		emit(d, s != NULL ? s : "*");
		d->chars++;
		d->spaced = false;
	}
	d->code = 1;
}

/* 2-means over v[0..n): true when they split in two groups whose centres are at least ratio4 / 4 apart */
static bool two_means(const uint16_t *v, unsigned n, uint32_t ratio4, uint32_t *c1, uint32_t *c2)
{
	if (n < 3u)
		return false;
	uint32_t lo = 0xFFFFu, hi = 0;
	for (unsigned i = 0; i < n; i++) {
		lo = v[i] < lo ? v[i] : lo;
		hi = v[i] > hi ? v[i] : hi;
	}
	if (lo == 0 || 4u * hi < ratio4 * lo)
		return false;
	uint32_t a = lo, b = hi;
	for (int it = 0; it < 4; it++) {
		uint32_t t = (a + b) / 2u, s1 = 0, s2 = 0, n1 = 0, n2 = 0;
		for (unsigned i = 0; i < n; i++) {
			if (v[i] < t) {
				s1 += v[i];
				n1++;
			} else {
				s2 += v[i];
				n2++;
			}
		}
		if (n1 == 0 || n2 == 0)
			return false;
		a = s1 / n1;
		b = s2 / n2;
	}
	*c1 = a;
	*c2 = b;
	return 4u * b >= ratio4 * a;
}

/* The recent marks hold dots and dashes: their two centres (dash 2..6 dots) */
static bool marks_split(const cwdec_t *d, uint32_t *c1, uint32_t *c2)
{
	return two_means(d->hist, d->hist_n, 8u, c1, c2) && *c2 <= 6u * *c1;
}

static bool is_dash(const cwdec_t *d, uint32_t m)
{
	uint32_t c1, c2;
	if (marks_split(d, &c1, &c2))
		return 2u * m >= c1 + c2;
	return 32u * m >= (uint32_t)d->dot16 + d->dash16;
}

static uint16_t clamp_dot(uint32_t dot16)
{
	return (uint16_t)(dot16 < DOT16_MIN ? DOT16_MIN : dot16 > DOT16_MAX ? DOT16_MAX : dot16);
}

/* the dash 2..4.5 dots: the one not just heard follows the other */
static void keep_ratio(cwdec_t *d, bool dash_heard)
{
	if (dash_heard && (d->dash16 < 2u * d->dot16 || 2u * d->dash16 > 9u * d->dot16))
		d->dot16 = clamp_dot(d->dash16 / 3u);
	if (d->dash16 < 2u * d->dot16 || 2u * d->dash16 > 9u * d->dot16)
		d->dash16 = (uint16_t)(3u * d->dot16);
}

static void follow(uint16_t *avg, uint32_t m16)
{
	*avg = (uint16_t)((int32_t)*avg + ((int32_t)m16 - (int32_t)*avg) / 4);
}

/* A held tone (a carrier tone, a tuning call), not Morse */
static bool too_long(const cwdec_t *d, uint32_t m)
{
	return m * 16u > LONG_UNITS * unit16(d) && m * 16u > 2u * LONG_UNITS * DOT16_MIN;
}

static void hist_add(uint16_t *h, uint8_t *n, uint8_t *i, uint32_t v)
{
	h[*i] = (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
	*i = (uint8_t)((*i + 1u) % CWDEC_HIST);
	if (*n < CWDEC_HIST)
		(*n)++;
}

/* A mark heard (already in the history): dot or dash, the averages, the character's code */
static void mark_use(cwdec_t *d, uint32_t m)
{
	bool dash = is_dash(d, m);
	uint32_t m16 = m * 16u;
	if (dash) {
		follow(&d->dash16, m16 > 16u * 1000u ? 16u * 1000u : m16);
	} else {
		follow(&d->dot16, m16);
		d->dot16 = clamp_dot(d->dot16);
	}
	keep_ratio(d, dash);
	if (d->code == 0xFFFFu)
		return;
	if (d->code >= (1u << 12))
		d->code = 0xFFFFu;	/* longer than any code: "*" */
	else
		d->code = (uint16_t)(d->code << 1 | (dash ? 1u : 0u));
}

/* The word space limit, units x 16: between the two kinds of long gap when the recent ones show both; when they are
 * all alike but 4.5..6.5 units (a sender's Farnsworth spacing: long gaps between characters) half again their mean;
 * else 5 units (also for gaps all near 7: single-character words, "E E E") */
static uint32_t word16(const cwdec_t *d)
{
	uint32_t u = unit16(d), c1, c2, t = 5u * u;
	if (two_means(d->gaps, d->gaps_n, 6u, &c1, &c2)) {
		t = (c1 + c2) * 8u;	/* ms x 16 */
	} else if (d->gaps_n >= 3u) {
		uint32_t sum = 0;
		for (unsigned i = 0; i < d->gaps_n; i++)
			sum += d->gaps[i];
		uint32_t mean16 = 16u * sum / d->gaps_n;
		if (2u * mean16 > 9u * u && 2u * mean16 < 13u * u)
			t = 3u * mean16 / 2u;
	}
	return t < 4u * u ? 4u * u : t > 12u * u ? 12u * u : t;
}

/* A space of s16 so far (ms x 16, the weight added back): the character is out at 2 units */
static void decide(cwdec_t *d, int32_t s16)
{
	if (d->code != 1u && s16 >= (int32_t)(2u * unit16(d)))
		end_char(d);
}

static int32_t space16(const cwdec_t *d, uint32_t s)
{
	return (int32_t)((s > SPACE_MAX_MS ? SPACE_MAX_MS : s) * 16u) + weight16(d);
}

static bool long_gap(const cwdec_t *d, uint32_t s)
{
	return space16(d, s) >= (int32_t)(2u * unit16(d)) && s < SPACE_MAX_MS;
}

static void gap_add(cwdec_t *d, uint32_t s)
{
	if (long_gap(d, s))
		hist_add(d->gaps, &d->gaps_n, &d->gaps_i, (uint32_t)space16(d, s) / 16u);
}

/* A word space, if the space that ended was one (decided only when it ends, against the recent gaps; it shows as the
 * next character starts) */
static void word_space(cwdec_t *d, uint32_t s)
{
	if (!d->spaced && space16(d, s) >= (int32_t)word16(d)) {
		emit(d, " ");
		d->spaced = true;
	}
}

/* A space ended (the next mark starts) */
static void space_end(cwdec_t *d, uint32_t s)
{
	gap_add(d, s);
	word_space(d, s);
}

/* Learning ends: the speed from the marks so far (when they split), the gap history from the spaces kept, then the
 * kept marks and spaces decoded with them, so the first characters of a transmission come out right too */
static void lock(cwdec_t *d)
{
	uint32_t c1, c2;
	d->learn = false;
	if (marks_split(d, &c1, &c2)) {
		d->dot16 = clamp_dot(16u * c1);
		d->dash16 = (uint16_t)(16u * c2 > 16u * 1000u ? 16u * 1000u : 16u * c2);
		keep_ratio(d, true);
	}
	uint8_t n = d->lb_n;
	d->lb_n = 0;
	for (uint8_t i = 0; i < n; i++)
		if (!(d->lb[i] & 0x8000u))
			gap_add(d, d->lb[i]);
	for (uint8_t i = 0; i < n; i++) {
		uint16_t v = d->lb[i];
		if (v & 0x8000u) {
			mark_use(d, v & 0x7FFFu);
		} else {
			decide(d, space16(d, v));
			word_space(d, v);
		}
	}
}

/* Enough kept to decode: the marks split into dots and dashes, and three spaces between characters were seen (the
 * word spacing is judged against them) */
static bool learned(const cwdec_t *d)
{
	uint32_t c1, c2;
	if (!marks_split(d, &c1, &c2))
		return false;
	uint32_t u = 8u * (c2 - c1);	/* the unit those marks give, ms x 16 */
	int32_t w = 16 * (int32_t)c1 - (int32_t)u;
	unsigned gaps = 0;
	for (uint8_t i = 0; i < d->lb_n; i++)
		if (!(d->lb[i] & 0x8000u) && 16 * (int32_t)d->lb[i] + w >= (int32_t)(2u * u))
			gaps++;
	return gaps >= 3u;
}

/* A mark of m ms ended */
static void mark_end(cwdec_t *d, uint32_t m)
{
	if (too_long(d, m)) {
		d->code = 1;	/* what came before it is dropped too */
		d->lb_n = 0;
		return;
	}
	if (m > 4000u)
		m = 4000u;
	hist_add(d->hist, &d->hist_n, &d->hist_i, m);
	if (d->learn && d->lb_n >= CWDEC_LEARN)
		lock(d);	/* the buffer is full: decode with what there is */
	if (!d->learn) {
		mark_use(d, m);
		return;
	}
	d->lb[d->lb_n++] = (uint16_t)(m | 0x8000u);
	if (learned(d))
		lock(d);
}

/* A mark starts after a space of s ms */
static void mark_start(cwdec_t *d, uint32_t s)
{
	if (d->learn) {
		if (d->lb_n == 0)
			return;		/* the transmission's first mark */
		if (d->lb_n < CWDEC_LEARN) {
			d->lb[d->lb_n++] = (uint16_t)(s > 0x7FFFu ? 0x7FFFu : s);
			return;
		}
		lock(d);
		decide(d, space16(d, s));	/* it went by while learning: decided now */
	}
	space_end(d, s);
	if (space16(d, s) > (int32_t)(PAUSE_UNITS * unit16(d))) {
		/* a pause: the reply may come from another station at another speed, so it is learned afresh (kept and
		 * decoded once its speed shows: nothing is lost if it is the same one) */
		d->learn = true;
		d->lb_n = 0;
		d->hist_n = d->hist_i = 0;
		d->gaps_n = d->gaps_i = 0;
	}
}

void cwdec_flush(cwdec_t *d)
{
	if (d->learn && d->lb_n > 0)
		lock(d);
	end_char(d);
}

/* One millisecond of envelope */
static void key_ms(cwdec_t *d, int32_t e)
{
	d->ms++;
	bool gate = (int32_t)(d->hold_until - d->ms) > 0;
	bool raw = false;
	int32_t e16 = e * 16;
	if (!gate) {
		if (d->gate) {
			/* the tone has gone (3 s): what was being learned or received comes out, the next one is learned
			 * afresh (another station, another speed); the averages stay as a start */
			cwdec_flush(d);
			d->learn = true;
			d->hist_n = d->hist_i = 0;
			d->gaps_n = d->gaps_i = 0;
			d->spaced = true;
		}
		d->floor += (e16 - d->floor) / 64;	/* no tone: everything is the space level */
		d->peak = d->floor;
	} else {
		/* down above half way between the levels, up below 3/8; never within 3x (down) / 2x (up) the space level, which
		 * the noise's envelope rarely reaches (Rayleigh: 3x its mean about once in a thousand) */
		int32_t span = d->peak - d->floor, f = d->floor + 64;
		int32_t on = d->floor + span / 2, off = d->floor + span * 3 / 8;
		on = on > 3 * f ? on : 3 * f;
		off = off > 2 * f ? off : 2 * f;
		raw = d->key ? e16 > off : e16 > on;
		if (e16 > d->peak)
			d->peak += (e16 - d->peak) / 2;		/* a stronger signal: at once */
		else if (d->key)
			d->peak += (e16 - d->peak) / 16;	/* the mark level follows fading */
		/* and sinks towards the space level over 6 dots: a mark after a fade still keys */
		uint32_t tau = 6u * d->dot16 / 16u;
		d->peak -= (d->peak - d->floor) / (int32_t)(tau != 0 ? tau : 1u);
		if (!d->key && !raw)	/* a space, not the start of a mark not yet counted */
			d->floor += (e16 - d->floor) / 32;
		if (d->floor < 0)
			d->floor = 0;
	}
	d->gate = gate;
	if (raw != d->key) {
		uint32_t deb = d->dot16 / 64u;	/* a quarter dot */
		deb = deb < 3u ? 3u : deb > 15u ? 15u : deb;
		if (++d->run >= deb) {
			uint32_t at = d->ms - d->run;
			if (d->key)
				mark_end(d, at - d->edge);
			else
				mark_start(d, at - d->edge);
			d->key = raw;
			d->edge = at;
			d->run = 0;
		}
	} else {
		d->run = 0;
	}
	if (!d->key && !d->learn)
		decide(d, space16(d, d->ms - d->edge));
}

/* --- tone search --- */

static bool pair(uint8_t a, uint8_t b)
{
	return a != 0xFFu && b != 0xFFu && (unsigned)(a > b ? a - b : b - a) <= 1u;
}

/* The block's powers; the strongest bin if it is a tone, else 0xFF */
static uint8_t search(cwdec_t *d)
{
	int32_t pk = 0;
	for (unsigned i = 0; i < CWDEC_N; i++) {
		int32_t a = d->blk[i] < 0 ? -(int32_t)d->blk[i] : d->blk[i];
		pk = a > pk ? a : pk;
	}
	if (pk < 4)
		return 0xFFu;	/* silence */
	unsigned sh = 0;
	while ((pk >> sh) > 1023)
		sh++;		/* |x| <= 1023: the resonators stay within 2^18, c * s within 2^31 */
	uint8_t best = 0;
	for (unsigned k = 0; k < CWDEC_BINS; k++) {
		int32_t c = coef[k], s1 = 0, s2 = 0;
		for (unsigned i = 0; i < CWDEC_N; i++) {
			int32_t s0 = (d->blk[i] >> sh) + ((c * s1) >> 12) - s2;
			s2 = s1;
			s1 = s0;
		}
		int64_t p = (int64_t)s1 * s1 + (int64_t)s2 * s2 - (((int64_t)c * s1) >> 12) * s2;
		p = p < 0 ? 0 : p >> 8;
		d->pw[k] = p > 0xFFFFFFFF ? 0xFFFFFFFFu : (uint32_t)p;
		if (d->pw[k] > d->pw[best])
			best = (uint8_t)k;
	}
	uint64_t far = 0;
	unsigned n = 0;
	for (unsigned k = 0; k < CWDEC_BINS; k++) {
		if (k + FAR_BINS <= (unsigned)best || k >= (unsigned)best + FAR_BINS) {
			far += d->pw[k];
			n++;
		}
	}
	if (d->pw[best] == 0 || (uint64_t)d->pw[best] * n < (uint64_t)RATIO_TONE * far)
		return 0xFFu;
	return best;
}

/* A tone block: its shape into the average (afresh for a tone elsewhere), the tone by a parabola through the peak */
static void follow_tone(cwdec_t *d, uint8_t k)
{
	bool fresh = d->track == 0xFFu || (unsigned)(k > d->track ? k - d->track : d->track - k) > 2u;
	uint32_t top = d->pw[k];
	for (unsigned j = 0; j < CWDEC_BINS; j++) {
		uint32_t v = (uint32_t)(((uint64_t)d->pw[j] << 16) / top);
		v = v > 0xFFFFu ? 0xFFFFu : v;
		d->pavg[j] = fresh ? (uint16_t)v : (uint16_t)(d->pavg[j] + ((int32_t)v - (int32_t)d->pavg[j]) / 4);
	}
	uint8_t m = 0;
	for (unsigned j = 1; j < CWDEC_BINS; j++)
		if (d->pavg[j] > d->pavg[m])
			m = (uint8_t)j;
	d->track = m;
	int32_t f16 = (int32_t)(CWDEC_TONE_MIN + m * CWDEC_TONE_STEP) * 16;
	if (m > 0 && m + 1u < CWDEC_BINS) {
		int32_t a = d->pavg[m - 1], b = d->pavg[m], c = d->pavg[m + 1];
		int32_t den = 2 * (2 * b - a - c);
		if (den > 0)
			f16 += (int32_t)((int64_t)(c - a) * (CWDEC_TONE_STEP * 16) / den);	/* within half a bin */
	}
	d->tone16 = (uint16_t)f16;
	d->step = (uint32_t)(((uint64_t)f16 << 32) / (16u * CWDEC_FS));
}

/* The block mixed down by the tone: its envelope, a sample a millisecond */
static void mix(cwdec_t *d)
{
	unsigned k = d->dot16 >= 16u * 150u ? 6u : d->dot16 >= 16u * 70u ? 5u : 4u;
	for (unsigned i = 0, ms = 0; i < CWDEC_N; i++) {
		int32_t x = d->blk[i];
		int32_t vi = (x * sin_q15(d->phase + 0x40000000u)) >> 15, vq = (x * sin_q15(d->phase)) >> 15;
		d->phase += d->step;
		d->li1 += ((vi * 16) - d->li1) >> k;
		d->lq1 += ((vq * 16) - d->lq1) >> k;
		d->li2 += (d->li1 - d->li2) >> k;
		d->lq2 += (d->lq1 - d->lq2) >> k;
		if ((i & 7u) == 7u) {
			int32_t a = d->li2 / 16, b = d->lq2 / 16;
			d->env[ms++] = (uint16_t)isqrt((uint32_t)(a * a) + (uint32_t)(b * b));
		}
	}
}

static void block(cwdec_t *d)
{
	uint8_t k = search(d);
	d->kh[0] = d->kh[1];
	d->kh[1] = d->kh[2];
	d->kh[2] = k;
	/* the tone followed: a block next to another on the same bin, or one on the bin already followed (a lone block
	 * elsewhere is noise; it never moves the mixer) */
	if (pair(d->kh[1], k) || (d->track != 0xFFu && pair(d->track, k)))
		follow_tone(d, k);
	/* the block before this one: a tone there next to another (before or after) holds the gate */
	if (d->env_ok) {
		if (pair(d->kh[0], d->kh[1]) || pair(d->kh[1], d->kh[2]))
			d->hold_until = d->ms + CWDEC_BLOCK_MS + CWDEC_HOLD_MS;
		for (unsigned i = 0; i < CWDEC_BLOCK_MS; i++)
			key_ms(d, d->env[i]);
	}
	mix(d);
	d->env_ok = true;
}

void cwdec_audio(cwdec_t *d, const int16_t *s, uint16_t n)
{
	while (n-- > 0) {
		d->blk[d->blk_n++] = *s++;
		if (d->blk_n == CWDEC_N) {
			block(d);
			d->blk_n = 0;
		}
	}
}

uint8_t cwdec_wpm(const cwdec_t *d)
{
	uint32_t u = unit16(d);
	if (d->chars == 0 || u == 0)
		return 0;
	uint32_t w = (1200u * 16u + u / 2u) / u;
	return (uint8_t)(w > 99u ? 99u : w);
}

uint16_t cwdec_tone(const cwdec_t *d)
{
	return (int32_t)(d->hold_until - d->ms) > 0 ? (uint16_t)((d->tone16 + 8u) / 16u) : 0;
}

bool cwdec_key(const cwdec_t *d)
{
	return d->key;
}
