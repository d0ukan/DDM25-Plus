/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus P25 trunk following engine (p25trunk.h). Message layouts from TIA-102.AABC (trunking control channel
 * messages, standard manufacturer ID 0x00/0x01; vendor TSBKs reuse the opcodes and are ignored). No RTOS, no hardware. */
#include "config.h"
#if CONFIG_P25_TRUNK
#include <string.h>
#include "radio/p25trunk.h"

enum {
	OP_GRP_V_CH_GRANT = 0x00, OP_GRP_V_CH_GRANT_UPDT = 0x02, OP_GRP_V_CH_GRANT_UPDT_EXP = 0x03,
	OP_IDEN_UP_TDMA = 0x33, OP_IDEN_UP_VU = 0x34, OP_RFSS_STS_BCST = 0x3A, OP_NET_STS_BCST = 0x3B, OP_IDEN_UP = 0x3D,
	SVC_ENCRYPTED = 0x40,	/* service options: protected (encrypted) call */
};

static p25tk_view_t v;
/* The channel plan: base (5 Hz units on air), spacing (125 Hz units), TDMA slots per carrier (0 = FDMA) */
static struct { uint32_t base_hz; uint16_t step_125; uint8_t valid, slots; } iden[P25TK_IDENS];
static uint32_t grant_ms, tuned_ms, last_tsbk_ms, rate_ms, rate_at;
static struct { uint32_t until_ms; uint16_t tg, ch; bool any_grant; } holdoff;
static volatile uint32_t seq;		/* odd while written (snapshot retries) */
static volatile uint32_t reads;		/* snapshots taken: the screen still looks */
static uint32_t reads_seen, read_ms, serial, bad_base;
static bool bad_based;

static void begin_write(void) { seq++; __asm__ volatile("" ::: "memory"); }
static void end_write(void) { __asm__ volatile("" ::: "memory"); seq++; }

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

uint32_t p25tk_channel_hz(uint16_t ch, bool *tdma)
{
	unsigned id = ch >> 12, number = ch & 0xFFFu;
	if (tdma)
		*tdma = false;
	if (!iden[id].valid)
		return 0;
	if (iden[id].slots) {
		if (tdma)
			*tdma = true;
		number /= iden[id].slots;	/* a TDMA channel number counts slots */
	}
	uint64_t hz = (uint64_t)iden[id].base_hz + (uint64_t)iden[id].step_125 * 125u * number;
	return hz > 0xFFFFFFFFu ? 0 : (uint32_t)hz;
}

static void set_iden(const uint8_t *b, uint8_t slots)
{
	unsigned id = b[2] >> 4;
	uint64_t base = ((uint64_t)b[6] << 24 | (uint64_t)b[7] << 16 | (uint64_t)b[8] << 8 | b[9]) * 5u;
	uint16_t step = (uint16_t)((b[4] & 3u) << 8 | b[5]);
	if (base > 0xFFFFFFFFu || step == 0)
		return;
	iden[id].base_hz = (uint32_t)base;
	iden[id].step_125 = step;
	iden[id].slots = slots;
	iden[id].valid = 1;
}

/* A grant heard: into the recent list (newest first, one entry per group and channel) */
static p25tk_grant_t *note(uint16_t tg, uint16_t ch, uint32_t src, uint8_t flags, uint32_t now_ms)
{
	unsigned i = 0;
	while (i < v.n && !(v.recent[i].tg == tg && v.recent[i].ch == ch))
		i++;
	p25tk_grant_t g;
	if (i < v.n) {
		g = v.recent[i];
	} else {
		memset(&g, 0, sizeof g);
		if (v.n < P25TK_RECENT)
			v.n++;
		i = v.n - 1u;
	}
	memmove(&v.recent[1], &v.recent[0], i * sizeof g);
	g.tg = tg;
	g.ch = ch;
	g.hz = p25tk_channel_hz(ch, NULL);
	if (src)
		g.src = src;
	g.ms = now_ms;
	g.flags = flags;
	v.recent[0] = g;
	return &v.recent[0];
}

static void grant(uint16_t tg, uint16_t ch, uint32_t src, uint8_t opts, bool update, uint32_t now_ms)
{
	if (tg == 0)
		return;
	bool tdma;
	uint32_t hz = p25tk_channel_hz(ch, &tdma);
	uint8_t flags;
	if (v.state == P25TK_VOICE && tg == v.call.tg && ch == v.call.ch)
		flags = P25TK_G_FOLLOWED;	/* the call followed (heard again before the receiver left) */
	else if (v.state != P25TK_CONTROL)
		flags = P25TK_G_BUSY;
	else if (!v.all && v.follow_tg && tg != v.follow_tg)
		flags = P25TK_G_OTHER;
	else if (opts & SVC_ENCRYPTED)
		flags = P25TK_G_ENC;
	else if (hz == 0)
		flags = P25TK_G_NO_PLAN;
	else if (tdma)
		flags = P25TK_G_TDMA;
	else if (!p25tk_band_ok(hz))
		flags = P25TK_G_BAND;
	else if ((update || holdoff.any_grant) && tg == holdoff.tg && ch == holdoff.ch &&
		 (int32_t)(holdoff.until_ms - now_ms) > 0)
		flags = P25TK_G_BUSY;	/* the call just left (its updates still come) */
	else
		flags = P25TK_G_FOLLOWED;
	p25tk_grant_t *g = note(tg, ch, src, flags, now_ms);
	if (flags != P25TK_G_FOLLOWED || v.state == P25TK_VOICE)
		return;
	v.call = *g;
	v.call.src = src;	/* this grant's (an update has none: the voice's link control may tell) */
	v.state = P25TK_VOICE;
	v.calls++;
	grant_ms = now_ms;
	tuned_ms = 0;
}

void p25tk_tsbk(const uint8_t b[10], uint32_t now_ms)
{
	if (v.state == P25TK_OFF)
		return;
	begin_write();
	v.tsbks++;
	if (v.state == P25TK_HUNT)
		v.state = P25TK_CONTROL;	/* locked */
	if (v.state == P25TK_CONTROL)
		last_tsbk_ms = now_ms;
	uint8_t op = b[0] & 0x3Fu;
	bool standard = b[1] <= 0x01u && !(b[0] & 0x40u);	/* standard MFID, not protected */
	if (standard) {
		switch (op) {
		case OP_GRP_V_CH_GRANT:
			grant(be16(b + 5), be16(b + 3), (uint32_t)b[7] << 16 | (uint32_t)b[8] << 8 | b[9], b[2], false, now_ms);
			break;
		case OP_GRP_V_CH_GRANT_UPDT:
			grant(be16(b + 4), be16(b + 2), 0, 0, true, now_ms);
			if (be16(b + 8) != be16(b + 4) || be16(b + 6) != be16(b + 2))
				grant(be16(b + 8), be16(b + 6), 0, 0, true, now_ms);
			break;
		case OP_GRP_V_CH_GRANT_UPDT_EXP:	/* the channel the FNE transmits on: Channel (T) */
			grant(be16(b + 8), be16(b + 4), 0, b[2], true, now_ms);
			break;
		case OP_IDEN_UP_VU:
		case OP_IDEN_UP:
			set_iden(b, 0);
			break;
		case OP_IDEN_UP_TDMA: {
			static const uint8_t slots[16] = { 1, 1, 1, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
			set_iden(b, slots[b[2] & 15u]);
			break;
		}
		case OP_RFSS_STS_BCST:
			v.sysid = (uint16_t)((b[3] & 0x0Fu) << 8 | b[4]);
			v.rfss = b[5];
			v.site = b[6];
			v.have_site = true;
			break;
		case OP_NET_STS_BCST:
			v.wacn = (uint32_t)b[3] << 12 | (uint32_t)b[4] << 4 | b[5] >> 4;
			v.sysid = (uint16_t)((b[5] & 0x0Fu) << 8 | b[6]);
			v.have_net = true;
			break;
		default:
			break;
		}
	}
	end_write();
	serial++;
}

/* Back to the control channel; the call's grants are held off a while (longer when no call was found) */
static void call_over(bool missed, uint32_t now_ms)
{
	holdoff.tg = v.call.tg;
	holdoff.ch = v.call.ch;
	holdoff.any_grant = missed;
	holdoff.until_ms = now_ms + (missed ? P25TK_MISS_HOLDOFF_MS : P25TK_HOLDOFF_MS);
	v.call.ms = now_ms;
	v.state = P25TK_CONTROL;
	last_tsbk_ms = now_ms;		/* the lost timer starts again */
}

uint32_t p25tk_poll(const p25tk_rx_t *rx, uint32_t now_ms)
{
	if (v.state == P25TK_OFF)
		return 0;
	if (reads != reads_seen) {
		reads_seen = reads;
		read_ms = now_ms;
	} else if (now_ms - read_ms >= P25TK_IDLE_MS) {
		p25tk_stop();	/* nobody looks: the radio task tunes the channel again */
		return 0;
	}
	uint8_t was = v.state;
	bool show = false;
	if (!bad_based) {
		bad_based = true;
		bad_base = rx->bad;	/* the receive's count runs since start-up: from here on */
	}
	begin_write();
	if (rx->bad - bad_base != v.bad) {
		v.bad = rx->bad - bad_base;
		show = true;
	}
	if (v.state != P25TK_VOICE && rx->on_hz == v.cc_hz && rx->frames && rx->nac != v.heard_nac) {
		v.heard_nac = rx->nac;
		show = true;
	}
	if (now_ms - rate_ms >= 1000u) {
		v.rate = (uint16_t)(v.tsbks - rate_at > 0xFFFFu ? 0xFFFFu : v.tsbks - rate_at);
		rate_at = v.tsbks;
		rate_ms = now_ms;
		show = true;
	}
	if (v.state == P25TK_CONTROL && now_ms - last_tsbk_ms >= P25TK_LOST_MS) {
		v.state = P25TK_HUNT;	/* lost: listen on for it */
		v.lost++;
	} else if (v.state == P25TK_VOICE) {
		if (rx->on_hz != v.call.hz) {
			if (now_ms - grant_ms >= P25TK_MOVE_MS)
				call_over(true, now_ms);	/* the receiver never got there */
		} else {
			if (!tuned_ms)
				tuned_ms = now_ms;
			if (rx->src && !v.call.src && rx->tg == v.call.tg) {
				v.call.src = rx->src;	/* late entry: the source from the voice's link control */
				show = true;
			}
			if (rx->frames)
				v.call.ms = rx->last_ms;
			if (rx->terms)
				call_over(false, now_ms);	/* a terminator: the call (this transmission) is over */
			else if (rx->frames ? now_ms - rx->last_ms >= P25TK_FADE_MS : now_ms - tuned_ms >= P25TK_NO_CALL_MS)
				call_over(!rx->frames, now_ms);
		}
	}
	end_write();
	if (show || v.state != was)
		serial++;
	return p25tk_hz();
}

uint32_t p25tk_hz(void)
{
	return v.state == P25TK_OFF ? 0 : v.state == P25TK_VOICE ? v.call.hz : v.cc_hz;
}

void p25tk_start(uint32_t cc_hz, uint16_t nac, uint16_t tg, bool all, uint32_t now_ms)
{
	begin_write();
	memset(&v, 0, sizeof v);
	v.state = P25TK_HUNT;
	v.cc_hz = cc_hz;
	v.nac = nac;
	v.heard_nac = 0xFFFFu;
	v.follow_tg = tg;
	v.all = all || tg == 0;
	end_write();
	serial++;
	memset(iden, 0, sizeof iden);
	memset(&holdoff, 0, sizeof holdoff);
	grant_ms = tuned_ms = rate_at = 0;
	bad_based = false;
	last_tsbk_ms = rate_ms = read_ms = now_ms;
	reads_seen = reads;
}

void p25tk_stop(void)
{
	begin_write();
	v.state = P25TK_OFF;
	end_write();
	serial++;
}

void p25tk_follow(uint16_t tg, bool all)
{
	begin_write();
	v.follow_tg = tg;
	v.all = all || tg == 0;
	end_write();
	serial++;
}

bool p25tk_on(void)
{
	return v.state != P25TK_OFF;
}

uint16_t p25tk_nac(void)
{
	return v.nac;
}

uint32_t p25tk_serial(void)
{
	return serial;
}

bool p25tk_snapshot(p25tk_view_t *out)
{
	reads++;
	for (unsigned tries = 0; tries < 8u; tries++) {
		uint32_t a = seq;
		__asm__ volatile("" ::: "memory");
		if (a & 1u)
			continue;
		memcpy(out, &v, sizeof *out);
		__asm__ volatile("" ::: "memory");
		if (seq == a)
			return true;
	}
	return false;
}
#endif
