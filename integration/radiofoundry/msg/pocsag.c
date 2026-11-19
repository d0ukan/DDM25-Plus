/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus POCSAG decoder (pocsag.h; the format and its source, ITU-R M.584-2, are described there). Written for
 * this project from the Recommendation; no code from other decoders. */
#include "config.h"
#if CONFIG_MSG_RX
#include <string.h>
#include "pocsag.h"

/* --- demodulator --- */
#define LEVEL_SHIFT	6	/* the two levels follow their side with a time constant of 64 samples (8 ms) */
#define PK_ATTACK	2	/* the peak swing from the middle follows a rise within a few samples ... */
#define PK_DECAY	9	/* ... and falls with a time constant of 512 samples (64 ms) */
#define EDGE_HYST_SHIFT	1	/* the edge slicer's hysteresis: half the peak swing on each side of the middle */
#define LEVEL_HYST_SHIFT 3	/* the level slicer's: an eighth of the distance between the two levels */
#define PLL_DIV		4	/* each edge moves the clock a quarter of its error */
#define SNAP_BITS	10u	/* bits without an edge after which the next one sets the clock (not inside a batch) */

/* --- framing --- */
#define PRE_BITS	32u	/* alternating bits that make a preamble */
#define PRE_GAP		40u	/* the first sync ends at most this many bits after the preamble */
#define SYNC_ERRS	2u	/* the first sync after a preamble */
#define NEXT_SYNC_ERRS	4u	/* the sync expected after a batch */
#define GRACE_BITS	1088u	/* two batches */
#define BATCH_WORDS	16u

/* 2^32 x rate / 8000 */
static const uint32_t bit_step[POCSAG_RATES] = { 274877907u, 644245094u, 1288490189u };
/* one-pole low pass, time constant 2^k samples: about a quarter bit at 512 and 1200 bit/s, none at 2400 */
static const uint8_t lp_shift[POCSAG_RATES] = { 2, 1, 0 };

/* BCH(31,21): x^10 + x^9 + x^8 + x^6 + x^5 + x^3 + 1 */
#define BCH_POLY	0x769u
/* x^k mod the generator: the syndrome of an error in bit k of the 31-bit code word (codeword bit k + 1) */
static const uint16_t syn_bit[31] = {
	0x001, 0x002, 0x004, 0x008, 0x010, 0x020, 0x040, 0x080, 0x100, 0x200, 0x369, 0x1BB, 0x376, 0x185, 0x30A, 0x17D,
	0x2FA, 0x29D, 0x253, 0x3CF, 0x0F7, 0x1EE, 0x3DC, 0x0D1, 0x1A2, 0x344, 0x1E1, 0x3C2, 0x0ED, 0x1DA, 0x3B4,
};

/* Numeric characters (M.584 Table 2): 0-9, spare, urgency, space, hyphen, and the two brackets (some decoders show
 * them as parentheses) */
static const char numeric_chars[17] = "0123456789*U -][";

static unsigned popcount32(uint32_t v)
{
	unsigned n = 0;
	while (v != 0) {
		v &= v - 1u;
		n++;
	}
	return n;
}

static uint32_t remainder31(uint32_t v)
{
	for (int i = 30; i >= 10; i--)
		if (v & (1u << i))
			v ^= BCH_POLY << (i - 10);
	return v & 0x3FFu;
}

uint32_t pocsag_encode(uint32_t info21)
{
	uint32_t v = (info21 & 0x1FFFFFu) << 10;
	v |= remainder31(v);
	return v << 1 | (popcount32(v) & 1u);
}

int pocsag_correct(uint32_t *cw)
{
	uint32_t c = *cw;
	uint32_t s = remainder31(c >> 1);
	bool parity_ok = (popcount32(c) & 1u) == 0;
	if (s == 0) {
		if (parity_ok)
			return 0;
		*cw = c ^ 1u;	/* the parity bit itself */
		return 1;
	}
	for (int i = 0; i < 31; i++) {
		if (syn_bit[i] != s)
			continue;
		c ^= 2u << i;
		if ((popcount32(c) & 1u) == 0) {
			*cw = c;
			return 1;
		}
		*cw = c ^ 1u;
		return 2;
	}
	for (int i = 0; i < 31; i++) {
		for (int j = i + 1; j < 31; j++) {
			if ((uint32_t)(syn_bit[i] ^ syn_bit[j]) != s)
				continue;
			c ^= (2u << i) | (2u << j);
			if ((popcount32(c) & 1u) != 0)
				return -1;	/* a third error */
			*cw = c;
			return 2;
		}
	}
	return -1;
}

/* --- messages --- */

static void end_msg(pocsag_t *p)
{
	if (!p->in_msg)
		return;
	p->in_msg = false;
	p->msgs++;
	p->m.bits = p->bits;
	if (p->fn != NULL)
		p->fn(p->ctx, &p->m);
}

static void append(pocsag_t *p, uint32_t cw)
{
	for (int k = 30; k >= 11; k--) {
		uint16_t n = p->m.nbits;
		if (n >= POCSAG_BITS_MAX) {
			p->m.cut = true;
			return;
		}
		uint8_t mask = (uint8_t)(0x80u >> (n & 7u));
		if ((cw >> k) & 1u)
			p->bits[n >> 3] |= mask;
		else
			p->bits[n >> 3] &= (uint8_t)~mask;
		p->m.nbits = (uint16_t)(n + 1u);
	}
}

/* Codeword k of a batch, corrected as far as it goes (e errors, -1 = more than the code corrects) */
static void codeword(pocsag_t *p, uint32_t c, int e, uint8_t k)
{
	uint8_t frame = (uint8_t)(k >> 1);
	if (e < 0) {
		p->uncorrectable++;
		if ((c & 0x80000000u) == 0) {
			end_msg(p);	/* most likely the idle codeword or the next address: the message is over */
		} else if (p->in_msg) {
			p->m.errors = true;
			append(p, c);	/* as it came: the message shows where it went wrong */
		}
		return;
	}
	p->corrected += (uint32_t)e;
	if (c == POCSAG_IDLE) {
		end_msg(p);
		return;
	}
	if ((c & 0x80000000u) == 0) {
		end_msg(p);
		p->in_msg = true;
		p->m.capcode = ((c >> 13) & 0x3FFFFu) << 3 | frame;
		p->m.function = (uint8_t)((c >> 11) & 3u);
		p->m.rate = (uint8_t)p->rate;
		p->m.errors = p->m.cut = false;
		p->m.nbits = 0;
		return;
	}
	if (p->in_msg)
		append(p, c);
}

static void pend_out(pocsag_t *p)
{
	if (!p->pend)
		return;
	p->pend = false;
	codeword(p, p->pend_cw, p->pend_e, p->pend_k);
}

/* The transmission ended (or the batches lost their place): what was being received goes out */
static void lose(pocsag_t *p)
{
	pend_out(p);
	end_msg(p);
	p->grace_rate = p->rate;
	p->grace = GRACE_BITS;
	p->rate = -1;
}

/* Codeword k of the batch from slicer r. While both slicers of the rate follow the batches, each codeword comes
 * twice, a bit or so apart: the first waits for the other, and the one with fewer errors is taken */
static void offer(pocsag_t *p, uint8_t r, uint8_t k, uint32_t cw)
{
	int e = pocsag_correct(&cw);
	if (p->pend) {
		if (p->pend_v != r && p->pend_k == k) {
			p->pend = false;
			bool mine = e >= 0 && (p->pend_e < 0 || e < p->pend_e);
			codeword(p, mine ? cw : p->pend_cw, mine ? e : p->pend_e, k);
			return;
		}
		bool behind = p->pend_v != r && k < p->pend_k;
		pend_out(p);
		if (behind)
			return;		/* the other slicer slipped: its codewords are out of place */
	}
	if (p->d[r ^ 1u].word < 0) {
		codeword(p, cw, e, k);
		return;
	}
	p->pend = true;
	p->pend_v = r;
	p->pend_k = k;
	p->pend_cw = cw;
	p->pend_e = (int8_t)e;
}

/* A bit of the batches slicer r follows */
static void take(pocsag_t *p, uint8_t r)
{
	pocsag_demod_t *d = &p->d[r];
	if (++d->nbit < 32u)
		return;
	d->nbit = 0;
	uint32_t cw = d->inv ? ~d->sr : d->sr;
	if (d->word >= (int8_t)BATCH_WORDS) {
		if (popcount32(cw ^ POCSAG_SYNC) <= NEXT_SYNC_ERRS) {
			d->word = 0;
			p->batches++;
			return;
		}
		d->word = -1;
		if (p->d[r ^ 1u].word < 0)
			lose(p);	/* neither slicer has the next batch */
		return;
	}
	offer(p, r, (uint8_t)d->word, cw);
	d->word++;
}

static void bit_out(pocsag_t *p, uint8_t r, unsigned b)
{
	pocsag_demod_t *d = &p->d[r];
	if (d->quiet < 255u)
		d->quiet++;
	if (b != (unsigned)d->last) {
		if (d->alt < 255u)
			d->alt++;
	} else {
		if (d->alt >= PRE_BITS)
			d->since_pre = 0;	/* a preamble just ended */
		d->alt = 0;
	}
	d->last = b != 0;
	if (d->since_pre < 255u)
		d->since_pre++;
	d->sr = d->sr << 1 | b;
	if (d->word >= 0) {
		take(p, r);
		return;
	}
	int8_t rate = (int8_t)(r >> 1);
	if (p->rate >= 0 && p->rate != rate)
		return;		/* another rate has the transmission */
	bool grace = p->grace != 0 && p->grace_rate == rate;
	if (grace && (r & 1u) == 0)
		p->grace--;	/* counted on one slicer's bits */
	pocsag_demod_t *o = &p->d[r ^ 1u];
	unsigned lim = d->since_pre <= PRE_GAP || grace || p->rate == rate ? SYNC_ERRS : 0u;
	unsigned e = popcount32(d->sr ^ POCSAG_SYNC);
	bool inv;
	if (e <= lim)
		inv = false;
	else if (32u - e <= lim)
		inv = true;
	else
		return;
	if (p->rate == rate && !((o->word == 0 && o->nbit <= 1u) || (o->word == (int8_t)BATCH_WORDS && o->nbit >= 31u)))
		return;		/* joining the other slicer: only at its sync, so both count the same codewords */
	d->word = 0;
	d->nbit = 0;
	d->inv = inv;
	if (p->rate < 0) {
		p->rate = rate;
		p->grace = 0;
		p->syncs++;
		p->batches++;
	}
}

static void demod(pocsag_t *p, uint8_t r, int32_t x16)
{
	pocsag_demod_t *d = &p->d[r];
	uint8_t rate = (uint8_t)(r >> 1);
	d->lp += (x16 - d->lp) >> lp_shift[rate];
	int32_t y = d->lp;
	int32_t mid = (d->hi + d->lo) / 2;
	int32_t h;
	if (r & 1u) {
		/* edge slicer */
		int32_t v = y - mid;
		if (v < 0)
			v = -v;
		if (v > d->pk)
			d->pk += (v - d->pk) >> PK_ATTACK;
		else
			d->pk -= d->pk >> PK_DECAY;
		h = d->pk >> EDGE_HYST_SHIFT;
	} else {
		/* level slicer */
		h = (d->hi - d->lo) >> LEVEL_HYST_SHIFT;
		if (h < 0)
			h = 0;
	}
	bool s = d->state;
	if (s && y < mid - h)
		s = false;
	else if (!s && y > mid + h)
		s = true;
	if (s)
		d->hi += (y - d->hi) >> LEVEL_SHIFT;
	else
		d->lo += (y - d->lo) >> LEVEL_SHIFT;
	bool edge = s != d->state;
	d->state = s;
	uint32_t old = d->ph;
	d->ph += bit_step[rate];
	if (d->ph < old) {
		bit_out(p, r, d->acc > 0 || (d->acc == 0 && s) ? 1u : 0u);
		d->acc = 0;
	}
	d->acc = (int16_t)(d->acc + (s ? 1 : -1));
	if (edge) {
		if (d->quiet >= SNAP_BITS && d->word < 0)
			d->ph = bit_step[rate] / 2u;	/* the edge was about half a sample ago */
		else
			d->ph -= (uint32_t)((int32_t)d->ph / PLL_DIV);
		d->quiet = 0;
	}
}

void pocsag_init(pocsag_t *p, pocsag_fn fn, void *ctx)
{
	memset(p, 0, sizeof *p);
	for (unsigned r = 0; r < POCSAG_DEMODS; r++) {
		p->d[r].since_pre = 255u;
		p->d[r].word = -1;
	}
	p->rate = -1;
	p->grace_rate = -1;
	p->m.bits = p->bits;
	p->fn = fn;
	p->ctx = ctx;
}

void pocsag_audio(pocsag_t *p, const int16_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int32_t x16 = (int32_t)s[i] * 16;
		for (uint8_t r = 0; r < POCSAG_DEMODS; r++)
			demod(p, r, x16);
	}
}

void pocsag_flush(pocsag_t *p)
{
	pend_out(p);
	end_msg(p);
	for (unsigned r = 0; r < POCSAG_DEMODS; r++)
		p->d[r].word = -1;
	p->rate = -1;
	p->grace = 0;
}

/* --- text --- */

static unsigned msg_bit(const pocsag_msg_t *m, unsigned i)
{
	return (m->bits[i >> 3] >> (7u - (i & 7u))) & 1u;
}

/* w bits from bit at on, the first one the least significant */
static unsigned msg_chr(const pocsag_msg_t *m, unsigned at, unsigned w)
{
	unsigned c = 0;
	for (unsigned k = 0; k < w; k++)
		c |= msg_bit(m, at + k) << k;
	return c;
}

static void put_end(char *out, size_t size, size_t n)
{
	if (out == NULL || size == 0)
		return;
	while (n > 0 && out[n - 1] == ' ')
		n--;
	out[n] = '\0';
}

/* Alphanumeric: the characters that don't belong in a message (control codes); *chars the characters seen */
static unsigned alpha(const pocsag_msg_t *m, char *out, size_t size, unsigned *chars)
{
	size_t n = 0;
	unsigned bad = 0, seen = 0;
	for (unsigned at = 0; at + 7u <= m->nbits; at += 7u) {
		unsigned c = msg_chr(m, at, 7u);
		if (c == 0x03u || c == 0x04u || c == 0x17u)
			break;		/* ETX, EOT, ETB: the text ends */
		if (c == 0)
			continue;	/* NUL: fill */
		seen++;
		if (c == '\r' || c == '\n' || c == '\t') {
			c = ' ';
		} else if (c < 0x20u || c == 0x7Fu) {
			bad++;
			c = '?';
		}
		if (out != NULL && n + 1u < size)
			out[n++] = (char)c;
	}
	put_end(out, size, n);
	*chars = seen;
	return bad;
}

/* Numeric: the spare characters (0xA, not used in messages) */
static unsigned numeric(const pocsag_msg_t *m, char *out, size_t size)
{
	size_t n = 0;
	unsigned bad = 0;
	for (unsigned at = 0; at + 4u <= m->nbits; at += 4u) {
		unsigned c = msg_chr(m, at, 4u);
		if (c == 0xAu)
			bad++;
		if (out != NULL && n + 1u < size)
			out[n++] = numeric_chars[c];
	}
	put_end(out, size, n);
	return bad;
}

uint8_t pocsag_text(const pocsag_msg_t *m, char *out, size_t size)
{
	if (size == 0)
		return POCSAG_TONE;
	out[0] = '\0';
	if (m->nbits == 0)
		return POCSAG_TONE;
	unsigned chars;
	unsigned abad = alpha(m, NULL, 0, &chars);
	unsigned nbad = numeric(m, NULL, 0);
	bool text;
	if (m->function == 0)
		text = nbad != 0 && chars >= 2u && abad == 0;	/* numeric, unless it reads as clean text */
	else
		text = !(abad * 4u > chars && nbad == 0);	/* text, unless it is mostly control codes */
	if (text) {
		alpha(m, out, size, &chars);
		return POCSAG_ALPHA;
	}
	numeric(m, out, size);
	return POCSAG_NUMERIC;
}
#endif
