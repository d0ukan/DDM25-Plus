/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus FM caller ID decoders: MDC1200, FleetSync I, DTMF (fmid.h; the formats and their sources are described
 * there). Written for this project from the published descriptions; no code from other decoders. */
#include "config.h"
#if CONFIG_FM_ID
#include <string.h>
#include "fmid.h"

/* --- front end --- */
#define HP_A_Q12	3315	/* one-pole high pass at about 300 Hz: a = 1 / (1 + 2 pi 300 / 8000) */
#define BIT_STEP	644245094u	/* 2^32 x 1200 / 8000 */
#define SNAP_QUIET	10u	/* bits without a zero crossing after which the next one sets the clock */
#define PLL_DIV		2	/* each crossing moves the clock half its error (FleetSync's preamble is short) */
#define DISC_SHIFT	6	/* the discriminator product (two int16) to 24 bits: a bit's sum stays in int32 */
#define FS_BP_B0	5141	/* the band pass (RBJ, center 1470 Hz, Q 1), Q14; b1 = 0, b2 = -b0 */
#define FS_BP_A1	(-9092)
#define FS_BP_A2	6101

/* --- MDC1200 --- */
#define MDC_SYNC	0x07092A446FULL
#define MDC_SYNC_ERRS	3u	/* sync bits allowed wrong (the CRC follows) */
#define MDC_BITS	112u

/* --- FleetSync --- */
#define FS_SYNC		0xA23Eu
#define FS_PRE_BITS	24u	/* the preamble's bits checked before the sync */
#define FS_PRE_ERRS	3u
#define FS_SYNC_ERRS	1u
#define FS_LEAD		4u	/* bits between the sync and the first word */
#define FS_BITS		(FS_LEAD + 64u)

/* --- DTMF --- */
/* 2 cos(2 pi f / 8000) in Q14 at the tones themselves: 697 770 852 941, 1209 1336 1477 1633 Hz */
static const int32_t dtmf_coef[8] = { 27980, 26956, 25701, 24219, 19073, 16325, 13085, 9315 };
static const char dtmf_keys[4][4] = {
	{ '1', '2', '3', 'A' }, { '4', '5', '6', 'B' }, { '7', '8', '9', 'C' }, { '*', '0', '#', 'D' },
};
#define DTMF_MIN_E	((uint64_t)FMID_DTMF_N * 400u)	/* block energy floor (input >> 2): an rms of 80 at the input */
#define DTMF_SUM_Q8	115u	/* the two tones hold at least 45 % of the block's energy */
#define DTMF_ONE_Q8	26u	/* and each at least 10 % */
#define DTMF_GROUP	4	/* each beats the others of its group by 6 dB */
#define DTMF_TWIST_X10	63	/* the two within 8 dB of each other */

static unsigned popcount64(uint64_t v)
{
	unsigned n = 0;
	while (v != 0) {
		v &= v - 1u;
		n++;
	}
	return n;
}

static void emit(fmid_t *f, const fmid_event_t *e)
{
	if (f->fn != NULL)
		f->fn(f->ctx, e);
}

void fmid_init(fmid_t *f, fmid_fn fn, void *ctx)
{
	memset(f, 0, sizeof *f);
	f->fn = fn;
	f->ctx = ctx;
}

/* --- MDC1200 --- */

uint16_t fmid_mdc_crc(const uint8_t *p, size_t n)
{
	uint16_t crc = 0;
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
	}
	return (uint16_t)(crc ^ 0xFFFFu);
}

static unsigned data_bit(const uint8_t *d, int n)
{
	return n < 0 ? 0u : (d[n >> 3] >> (n & 7)) & 1u;
}

void fmid_mdc_parity(const uint8_t data[7], uint8_t parity[7])
{
	memset(parity, 0, 7);
	for (int n = 0; n < 56; n++) {
		unsigned p = data_bit(data, n) ^ data_bit(data, n - 2) ^ data_bit(data, n - 5) ^ data_bit(data, n - 6);
		parity[n >> 3] |= (uint8_t)(p << (n & 7));
	}
}

/* Majority logic decoding with feedback: data bit m is in the checks m, m+2, m+5 and m+6 */
static void mdc_fec(uint8_t cw[14])
{
	uint8_t syn[7];
	fmid_mdc_parity(cw, syn);
	for (int i = 0; i < 7; i++)
		syn[i] ^= cw[7 + i];
	static const uint8_t taps[4] = { 0, 2, 5, 6 };
	for (int m = 0; m < 56; m++) {
		unsigned set = 0, avail = 0;
		for (int k = 0; k < 4; k++) {
			int c = m + taps[k];
			if (c < 56) {
				avail++;
				set += data_bit(syn, c);
			}
		}
		if (avail >= 3u && set * 4u >= avail * 3u) {
			cw[m >> 3] ^= (uint8_t)(1u << (m & 7));
			for (int k = 0; k < 4; k++) {
				int c = m + taps[k];
				if (c < 56)
					syn[c >> 3] ^= (uint8_t)(1u << (c & 7));
			}
		}
	}
}

static bool mdc_check(const uint8_t cw[14])
{
	return fmid_mdc_crc(cw, 4) == (uint16_t)(cw[4] | (cw[5] << 8));
}

static void mdc_packet(fmid_t *f)
{
	uint8_t *cw = f->mdc_cw;
	if (!mdc_check(cw)) {
		mdc_fec(cw);
		if (!mdc_check(cw)) {
			f->mdc_crc_bad++;
			return;
		}
	}
	uint8_t op = cw[0];
	if (op != 0x01u && op != 0x00u)
		return;		/* other ops (status, call alert, radio check...) name another unit or need two packets */
	fmid_event_t e;
	memset(&e, 0, sizeof e);
	e.kind = op == 0x00u ? FMID_MDC_EMERG : FMID_MDC;
	e.op = op;
	e.arg = cw[1];
	e.unit = (uint16_t)((cw[2] << 8) | cw[3]);
	emit(f, &e);
}

static void mdc_bit(fmid_t *f, unsigned t)
{
	f->mdc_d ^= (t != 0);
	unsigned d = f->mdc_d;
	if (f->mdc_n != 0) {
		/* codeword bit i went out at k = (i % 7) * 16 + i / 7: k's is i = (k % 16) * 7 + k / 16 */
		unsigned k = (unsigned)(f->mdc_n - 1u);
		unsigned i = (k % 16u) * 7u + k / 16u;
		if (d ^ f->mdc_inv)
			f->mdc_cw[i >> 3] |= (uint8_t)(1u << (i & 7u));
		if (++f->mdc_n > MDC_BITS) {
			f->mdc_n = 0;
			mdc_packet(f);
			f->mdc_sr = 0;
		}
		return;
	}
	f->mdc_sr = ((f->mdc_sr << 1) | d) & 0xFFFFFFFFFFULL;
	unsigned err = popcount64(f->mdc_sr ^ MDC_SYNC);
	bool inv = false;
	if (err > MDC_SYNC_ERRS) {
		err = 40u - err;	/* against the complement */
		inv = true;
	}
	if (err <= MDC_SYNC_ERRS) {
		f->mdc_syncs++;
		f->mdc_inv = inv;
		f->mdc_n = 1;
		memset(f->mdc_cw, 0, sizeof f->mdc_cw);
	}
}

/* --- FleetSync I --- */

uint16_t fmid_fs_check(uint64_t b)
{
	uint16_t crc = 0;
	for (int i = 63; i >= 16; i--) {
		unsigned fb = ((crc >> 14) & 1u) ^ (unsigned)((b >> i) & 1u);
		crc = (uint16_t)((crc << 1) & 0x7FFFu);
		if (fb)
			crc ^= 0x6815u;
	}
	uint16_t check = (uint16_t)((crc ^ 1u) << 1);
	/* the last bit makes the block's parity even */
	unsigned par = (popcount64(b >> 16) + popcount64(check)) & 1u;
	return (uint16_t)(check | par);
}

static void fs_block(fmid_t *f)
{
	uint64_t b = f->fs_block;
	if (fmid_fs_check(b) != (uint16_t)(b & 0xFFFFu)) {
		f->fs_crc_bad++;
		return;
	}
	uint32_t w1 = (uint32_t)(b >> 32), w2 = (uint32_t)b;
	unsigned fleet = (w1 >> 8) & 0xFFu, unit = ((w1 & 0xFFu) << 4) | (w2 >> 28);
	if (fleet == 0 || unit == 0)
		return;
	fmid_event_t e;
	memset(&e, 0, sizeof e);
	e.kind = FMID_FLEETSYNC;
	e.fleet = (uint16_t)(fleet + 99u);
	e.unit = (uint16_t)(unit + 999u);
	emit(f, &e);
}

static void fs_bit(fmid_t *f, unsigned t)
{
	if (f->fs_n != 0) {
		if (f->fs_n > FS_LEAD)
			f->fs_block = (f->fs_block << 1) | (uint64_t)(t ^ f->fs_inv);
		if (++f->fs_n > FS_BITS) {
			f->fs_n = 0;
			fs_block(f);
			f->fs_sr = 0;
		}
		return;
	}
	f->fs_sr = ((f->fs_sr << 1) | t) & 0xFFFFFFFFFFULL;
	unsigned sync = (unsigned)(f->fs_sr & 0xFFFFu);
	uint64_t pre = (f->fs_sr >> 16) & ((1ULL << FS_PRE_BITS) - 1u);
	unsigned se = popcount64(sync ^ FS_SYNC);
	bool inv = false;
	if (se > FS_SYNC_ERRS) {
		se = 16u - se;
		inv = true;
	}
	if (se > FS_SYNC_ERRS)
		return;
	unsigned pe = popcount64(pre ^ (0xAAAAAAULL & ((1ULL << FS_PRE_BITS) - 1u)));
	if (pe > FS_PRE_BITS / 2u)
		pe = FS_PRE_BITS - pe;	/* the other phase of the alternation */
	if (pe > FS_PRE_ERRS)
		return;
	f->fs_syncs++;
	f->fs_inv = inv;
	f->fs_n = 1;
	f->fs_block = 0;
}

/* --- the bit stream --- */

static void bit(fmid_t *f, unsigned t)
{
	f->bits++;
	mdc_bit(f, t);
	fs_bit(f, t);
}

/* A bit: the sign of its sum */
static void slice(fmid_t *f)
{
	unsigned t = f->acc > 0;
	f->acc = 0;
	bit(f, t);
}

static void ffsk(fmid_t *f, int32_t x)
{
	/* band pass at 1470 Hz (Q 1, the same gain at 1200 and 1800 Hz): less noise into the discriminator, and it takes
	 * most of the tilt out of the spill between neighbor bits (1 dB of tilt left instead of 2.5 at a 900 Hz
	 * de-emphasis corner) */
	int32_t y = (FS_BP_B0 * (x - f->bp_x2) - FS_BP_A1 * f->bp_y1 - FS_BP_A2 * f->bp_y2) >> 14;
	f->bp_x2 = f->bp_x1;
	f->bp_x1 = x;
	f->bp_y2 = f->bp_y1;
	if (y > 32767)
		y = 32767;
	else if (y < -32768)
		y = -32768;
	f->bp_y1 = y;
	int16_t old = f->dl[f->dl_pos];
	f->dl[f->dl_pos] = (int16_t)y;
	f->dl_pos = (uint8_t)((f->dl_pos + 1u) & 3u);
	int32_t d = (y * (int32_t)old) >> DISC_SHIFT;
	int32_t sm = d + f->d1 + f->d2;
	f->d2 = f->d1;
	f->d1 = d;
	f->acc += sm;
	/* zero crossings, with a little hysteresis: noise near zero doesn't flip the side back and forth */
	int32_t mag = sm < 0 ? -sm : sm;
	f->level += (mag - f->level) >> 6;
	int32_t hyst = f->level >> 3;
	bool hi = f->hi ? sm > -hyst : sm > hyst;
	uint32_t was = f->ph;
	f->ph += BIT_STEP;
	if (f->ph < was) {	/* a bit boundary */
		slice(f);
		if (f->quiet < 255u)
			f->quiet++;
	}
	if (hi != f->hi) {
		f->hi = hi;
		/* a crossing belongs on a boundary: ph 0 */
		int32_t e = (int32_t)f->ph;
		if (f->quiet >= SNAP_QUIET) {
			/* past half a bit since the last boundary: that was a bit of the run (the clock ran on without crossings);
			 * less: the other tone's tail, dropped */
			if (e < 0)
				slice(f);
			f->ph = 0;
			f->acc = 0;
		} else {
			f->ph = (uint32_t)((int32_t)f->ph - e / PLL_DIV);
		}
		f->quiet = 0;
	}
}

/* --- DTMF --- */

/* coef (Q14) x s >> 14 in 32-bit multiplies: s = hi 2^14 + lo, lo in [0, 2^14) */
static int32_t mul_q14(int32_t coef, int32_t s)
{
	int32_t hi = s >> 14, lo = s & 0x3FFF;
	return coef * hi + ((coef * lo) >> 14);
}

static void dtmf_block(fmid_t *f)
{
	int64_t p[8];
	for (int k = 0; k < 8; k++) {
		int64_t s1 = f->g1[k], s2 = f->g2[k];
		int64_t v = s1 * s1 + s2 * s2 - ((dtmf_coef[k] * s1) >> 14) * s2;
		p[k] = v > 0 ? v : 0;
	}
	char digit = 0;
	uint64_t e = f->g_energy;
	if (e >= DTMF_MIN_E) {
		int r = 0, c = 4;
		for (int k = 1; k < 4; k++) {
			if (p[k] > p[r])
				r = k;
			if (p[4 + k] > p[c])
				c = 4 + k;
		}
		bool ok = true;
		for (int k = 0; k < 4; k++) {
			if ((k != r && p[k] * DTMF_GROUP > p[r]) || (4 + k != c && p[4 + k] * DTMF_GROUP > p[c]))
				ok = false;
		}
		/* each tone's share of the block energy, Q8: 2 P / (N E) for a sine on its bin is 1 */
		int64_t den = (int64_t)FMID_DTMF_N * (int64_t)e;
		int64_t fr = (p[r] << 9) / den, fc = (p[c] << 9) / den;
		if (ok && fr >= DTMF_ONE_Q8 && fc >= DTMF_ONE_Q8 && fr + fc >= DTMF_SUM_Q8 &&
		    p[r] * DTMF_TWIST_X10 >= p[c] * 10 && p[c] * DTMF_TWIST_X10 >= p[r] * 10)
			digit = dtmf_keys[r][c - 4];
	}
	if (digit != 0 && digit == f->dtmf_blk && !f->dtmf_taken) {
		f->dtmf_taken = true;
		fmid_event_t ev;
		memset(&ev, 0, sizeof ev);
		ev.kind = FMID_DTMF;
		ev.digit = digit;
		emit(f, &ev);
	}
	if (digit != f->dtmf_blk)
		f->dtmf_taken = false;
	f->dtmf_blk = digit;
	memset(f->g1, 0, sizeof f->g1);
	memset(f->g2, 0, sizeof f->g2);
	f->g_energy = 0;
	f->g_n = 0;
}

static void dtmf(fmid_t *f, int32_t y)
{
	int32_t x = y >> 2;
	for (int k = 0; k < 8; k++) {
		int32_t s = x + mul_q14(dtmf_coef[k], f->g1[k]) - f->g2[k];
		f->g2[k] = f->g1[k];
		f->g1[k] = s;
	}
	f->g_energy += (uint32_t)(x * x);
	if (++f->g_n >= FMID_DTMF_N)
		dtmf_block(f);
}

void fmid_audio(fmid_t *f, const int16_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int32_t x = s[i];
		int32_t y = (HP_A_Q12 * (f->hp_y + x - f->hp_x)) >> 12;
		f->hp_x = x;
		if (y > 32767)
			y = 32767;
		else if (y < -32768)
			y = -32768;
		f->hp_y = y;
		ffsk(f, y);
		dtmf(f, y);
	}
}
#endif
