/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR Tier III trunk following engine (dmrtrunk.h). CSBK layouts in dmr_csbk.c (ETSI TS 102 361-4, standard
 * FID 0x00; vendor and protected CSBKs are ignored). No RTOS, no hardware. */
#include "config.h"
#if CONFIG_DMR_TRUNK
#include <string.h>
#include "radio/dmr_csbk.h"
#include "radio/dmrtrunk.h"

enum { KIND_CSBK = 3, KIND_MBC_HEADER = 4, KIND_MBC_CONT = 5 };	/* ETSI data types (hal_dmr_modem.h) */

static dmrtk_view_t v;
/* The channel plan: an LPCN's frequencies as sent. down_tx: the downlink is the "TX" one (learnt when the TSCC's own
 * frequency shows up as a TX frequency; the standard names them for the MS, so RX by default) */
static struct { uint32_t tx_hz, rx_hz; uint16_t lpcn; } plan[DMRTK_PLAN];
static bool down_tx;
/* An MBC header waiting for its continuation, per slot */
static struct { uint8_t b[10]; bool valid; } pend[2];
static const uint32_t *groups;
static uint16_t ngroups;
static uint32_t grant_ms, tuned_ms, voice_ms, last_ms, rate_ms, rate_at;
static bool voice_seen;
static struct { uint32_t until_ms, tg; uint16_t lpcn; uint8_t ts; } holdoff;
static volatile uint32_t seq;		/* odd while written (snapshot retries) */
static volatile uint32_t reads;		/* snapshots taken: the screen still looks */
static uint32_t reads_seen, read_ms, serial;

static void begin_write(void) { seq++; __asm__ volatile("" ::: "memory"); }
static void end_write(void) { __asm__ volatile("" ::: "memory"); seq++; }

uint32_t dmrtk_channel_hz(uint16_t lpcn)
{
	for (unsigned i = 0; i < v.plan_n; i++)
		if (plan[i].lpcn == lpcn)
			return down_tx ? plan[i].tx_hz : plan[i].rx_hz;
	return 0;
}

static void plan_set(const dmr_chan_t *c)
{
	if (c->lpcn == DMR_LPCN_ABSOLUTE)
		return;
	if (c->tx_hz == v.cc_hz && c->rx_hz != v.cc_hz)
		down_tx = true;		/* the TSCC itself, named from the base station's side */
	else if (c->rx_hz == v.cc_hz)
		down_tx = false;
	unsigned i = 0;
	while (i < v.plan_n && plan[i].lpcn != c->lpcn)
		i++;
	if (i == DMRTK_PLAN)
		i = DMRTK_PLAN - 1u;	/* full: the last entry gives way */
	else if (i == v.plan_n)
		v.plan_n++;
	plan[i].lpcn = c->lpcn;
	plan[i].tx_hz = c->tx_hz;
	plan[i].rx_hz = c->rx_hz;
	v.mbcs++;
}

static bool followed(uint32_t tg)
{
	if (v.all || tg == v.follow_tg)
		return true;
	for (uint16_t i = 0; i < ngroups; i++)
		if (groups[i] == tg)
			return true;
	return false;
}

/* A grant heard: into the recent list (newest first, one entry per target and channel) */
static dmrtk_grant_t *note(const dmrtk_grant_t *g)
{
	unsigned i = 0;
	while (i < v.n && !(v.recent[i].tg == g->tg && v.recent[i].lpcn == g->lpcn && v.recent[i].ts == g->ts))
		i++;
	if (i == v.n) {
		if (v.n < DMRTK_RECENT)
			v.n++;
		i = v.n - 1u;
	}
	memmove(&v.recent[1], &v.recent[0], i * sizeof *g);
	v.recent[0] = *g;
	return &v.recent[0];
}

static void grant(const dmr_csbk_t *c, uint16_t lpcn, uint32_t now_ms)
{
	if (c->target == 0)
		return;
	dmrtk_grant_t g;
	memset(&g, 0, sizeof g);
	g.tg = c->target;
	g.src = c->source;
	g.ms = now_ms;
	g.lpcn = lpcn;
	g.ts = c->ts;
	g.op = c->op;
	g.emergency = c->emergency;
	g.hz = lpcn == DMR_LPCN_ABSOLUTE ? 0 : dmrtk_channel_hz(lpcn);
	bool priv = c->op == CSBKO_PV_GRANT;
	if (v.state == DMRTK_VOICE && g.tg == v.call.tg && lpcn == v.call.lpcn && g.ts == v.call.ts)
		g.flags = DMRTK_G_FOLLOWED;	/* the call followed (heard again before the receiver left) */
	else if (v.state != DMRTK_CONTROL)
		g.flags = DMRTK_G_BUSY;
	else if (priv ? !v.all : !followed(g.tg))
		g.flags = priv ? DMRTK_G_PRIVATE : DMRTK_G_OTHER;
	else if (g.hz == 0)
		g.flags = DMRTK_G_NO_PLAN;
	else if (!dmrtk_band_ok(g.hz))
		g.flags = DMRTK_G_BAND;
	else if (g.tg == holdoff.tg && lpcn == holdoff.lpcn && g.ts == holdoff.ts &&
		 (int32_t)(holdoff.until_ms - now_ms) > 0)
		g.flags = DMRTK_G_BUSY;		/* the call just left (its grants still come) */
	else
		g.flags = DMRTK_G_FOLLOWED;
	const dmrtk_grant_t *r = note(&g);
	if (g.flags != DMRTK_G_FOLLOWED || v.state == DMRTK_VOICE)
		return;
	v.call = *r;
	v.state = DMRTK_VOICE;
	v.calls++;
	grant_ms = now_ms;
	tuned_ms = 0;
	voice_seen = false;
}

/* Back to the TSCC; the call's grants are held off a while (longer when no call was found, or it was encrypted) */
static void call_over(bool missed, uint32_t now_ms)
{
	holdoff.tg = v.call.tg;
	holdoff.lpcn = v.call.lpcn;
	holdoff.ts = v.call.ts;
	holdoff.until_ms = now_ms + (missed ? DMRTK_MISS_HOLDOFF_MS : DMRTK_HOLDOFF_MS);
	v.call.ms = now_ms;
	v.state = DMRTK_CONTROL;
	last_ms = now_ms;		/* the lost timer starts again */
}

/* One CSBK (or an MBC header with its continuation's channel parameters, chan) */
static void csbk(const dmr_csbk_t *c, const dmr_chan_t *chan, uint32_t now_ms)
{
	dmr_sys_t sys;
	switch (c->op) {
	case CSBKO_C_ALOHA:
	case CSBKO_C_BCAST:
		dmr_csbk_sys(c->syscode, &sys);
		v.model = sys.model;
		v.net = sys.net;
		v.site = sys.site;
		v.have_sys = true;
		if (c->op == CSBKO_C_BCAST)
			v.bcasts++;
		break;
	case CSBKO_TV_GRANT:
	case CSBKO_BTV_GRANT:
	case CSBKO_PV_GRANT:
		grant(c, c->lpcn == DMR_LPCN_ABSOLUTE && chan ? chan->lpcn : c->lpcn, now_ms);
		break;
	case CSBKO_P_CLEAR:
		/* Heard on the payload channel's own slot (the receiver hears nothing else there): it cleared down, whatever
		 * channel the CSBK names (the TSCC to return to on some systems) */
		if (v.state == DMRTK_VOICE)
			call_over(false, now_ms);
		break;
	case CSBKO_P_MAINT:
		if (v.state == DMRTK_VOICE)
			v.call.ms = now_ms;	/* the payload channel is kept */
		break;
	default:
		break;
	}
}

void __attribute__((noinline)) dmrtk_burst(uint8_t kind, int8_t ts, const uint8_t b[12], uint32_t now_ms)
{
	if (v.state == DMRTK_OFF)
		return;
	unsigned slot = ts == 2 ? 1u : 0u;
	dmr_csbk_t c;
	begin_write();
	if (kind == KIND_MBC_CONT) {
		/* The last block of a two-block MBC: its header's channel parameters (an intermediate block: the MBC is longer
		 * than this follower reads, dropped) */
		dmr_chan_t chan;
		if (pend[slot].valid && (b[0] & 0x80u) && dmr_csbk_chan(b, &chan)) {
			plan_set(&chan);
			dmr_csbk_parse(pend[slot].b, &c);
			csbk(&c, &chan, now_ms);
		}
		pend[slot].valid = false;
		goto done;
	}
	if (pend[slot].valid) {
		/* a header whose continuation never came: on its own (a grant's frequency stays unknown) */
		pend[slot].valid = false;
		dmr_csbk_parse(pend[slot].b, &c);
		csbk(&c, NULL, now_ms);
	}
	if (kind != KIND_CSBK && kind != KIND_MBC_HEADER)
		goto done;
	if (!dmr_csbk_crc_ok(b, kind == KIND_MBC_HEADER ? DMR_MBC_MASK : DMR_CSBK_MASK)) {
		v.bad++;
		goto done;
	}
	dmr_csbk_parse(b, &c);
	if (c.fid != 0 || c.protect)
		goto done;	/* a vendor's (Capacity / Connect Plus, ...) or protected: not Tier III as read here */
	switch (c.op) {
	case CSBKO_C_ALOHA: case CSBKO_C_BCAST: case CSBKO_P_MAINT: case CSBKO_P_CLEAR:
	case CSBKO_PV_GRANT: case CSBKO_TV_GRANT: case CSBKO_BTV_GRANT:
		break;
	default:
		goto done;
	}
	v.csbks++;
	if (v.state == DMRTK_HUNT)
		v.state = DMRTK_CONTROL;	/* locked */
	if (v.state == DMRTK_CONTROL)
		last_ms = now_ms;
	if (!c.last) {
		memcpy(pend[slot].b, b, 10);	/* an MBC header: its continuation follows in the next burst of the slot */
		pend[slot].valid = true;
	} else {
		csbk(&c, NULL, now_ms);
	}
done:
	end_write();
	serial++;
}

uint32_t dmrtk_poll(const dmrtk_rx_t *rx, uint32_t now_ms)
{
	if (v.state == DMRTK_OFF)
		return 0;
	if (reads != reads_seen) {
		reads_seen = reads;
		read_ms = now_ms;
	} else if (now_ms - read_ms >= DMRTK_IDLE_MS) {
		dmrtk_stop();	/* nobody looks: the radio task tunes the channel again */
		return 0;
	}
	uint8_t was = v.state;
	bool show = false;
	begin_write();
	if (now_ms - rate_ms >= 1000u) {
		v.rate = (uint16_t)(v.csbks - rate_at > 0xFFFFu ? 0xFFFFu : v.csbks - rate_at);
		rate_at = v.csbks;
		rate_ms = now_ms;
		show = true;
	}
	if (v.state == DMRTK_CONTROL && now_ms - last_ms >= DMRTK_LOST_MS) {
		v.state = DMRTK_HUNT;	/* lost: listen on for it */
		v.lost++;
	} else if (v.state == DMRTK_VOICE) {
		if (rx->on_hz != v.call.hz || rx->on_ts != v.call.ts) {
			if (now_ms - grant_ms >= DMRTK_MOVE_MS)
				call_over(true, now_ms);	/* the receiver never got there */
		} else {
			if (!tuned_ms)
				tuned_ms = now_ms;
			if (rx->call != DMRTK_RX_NONE) {
				voice_seen = true;
				voice_ms = v.call.ms = now_ms;
				if (rx->src && !v.call.src) {
					v.call.src = rx->src;	/* late entry: the source from the voice's link control */
					show = true;
				}
				if (rx->enc && rx->call == DMRTK_RX_MUTED) {
					/* encrypted, no key: listed, and the receiver goes back to the TSCC */
					v.call.flags = DMRTK_G_ENC;
					for (unsigned i = 0; i < v.n; i++)
						if (v.recent[i].tg == v.call.tg && v.recent[i].lpcn == v.call.lpcn &&
						    v.recent[i].ts == v.call.ts)
							v.recent[i].flags = DMRTK_G_ENC;
					call_over(true, now_ms);
				}
			} else if (voice_seen ? now_ms - voice_ms >= DMRTK_HANG_MS : now_ms - tuned_ms >= DMRTK_NO_CALL_MS) {
				call_over(!voice_seen, now_ms);
			}
		}
	}
	end_write();
	if (show || v.state != was)
		serial++;
	return dmrtk_hz();
}

uint32_t dmrtk_hz(void)
{
	return v.state == DMRTK_OFF ? 0 : v.state == DMRTK_VOICE ? v.call.hz : v.cc_hz;
}

uint8_t dmrtk_ts(void)
{
	return v.state == DMRTK_VOICE ? v.call.ts : 0;
}

void dmrtk_start(uint32_t cc_hz, uint32_t tg, bool all, const uint32_t *list, uint16_t nlist, uint32_t now_ms)
{
	begin_write();
	memset(&v, 0, sizeof v);
	v.state = DMRTK_HUNT;
	v.cc_hz = cc_hz;
	v.follow_tg = tg;
	v.all = all || tg == 0;
	end_write();
	serial++;
	memset(plan, 0, sizeof plan);
	memset(pend, 0, sizeof pend);
	memset(&holdoff, 0, sizeof holdoff);
	down_tx = false;
	groups = list;
	ngroups = list ? nlist : 0;
	grant_ms = tuned_ms = voice_ms = rate_at = 0;
	voice_seen = false;
	last_ms = rate_ms = read_ms = now_ms;
	reads_seen = reads;
}

void dmrtk_stop(void)
{
	begin_write();
	v.state = DMRTK_OFF;
	end_write();
	serial++;
}

void dmrtk_follow(uint32_t tg, bool all)
{
	begin_write();
	v.follow_tg = tg;
	v.all = all || tg == 0;
	end_write();
	serial++;
}

bool dmrtk_on(void)
{
	return v.state != DMRTK_OFF;
}

uint32_t dmrtk_serial(void)
{
	return serial;
}

bool dmrtk_snapshot(dmrtk_view_t *out)
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
