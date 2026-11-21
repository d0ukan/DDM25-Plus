/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: two whole sessions talking (core/radio/dota.c twice, as a_* the sender and b_* the
 * receiver; tools/test_dota.py renames one copy's entry points) over fake transports, on the real protocol, records,
 * import and codeplug over a simulated flash:
 *   - FM: the sender's tone buffers (preamble, then a frame per half queued with tx_more) are decoded bit by bit by the
 *     HDLC decoder, as the receiver's demodulator would, the AX.25 header checked, frames lost on request; the
 *     receiver hears only with its CTCSS detected;
 *   - DMR: the chained UDP datagrams to the receiver's ID, the answers back to the sender's;
 * and the receiver's storage jobs run as the UI would post them. Both radios share one codeplug here (a self
 * transfer): the copy lands in free slots and a new zone, the original is untouched. Cases: a zone into a new zone,
 * DOTA Clone, lossy links, a wrong code, the sender stopping (the receiver changes nothing), the PTT prompt. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/zone_edit.h"
#include "settings/settings.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"
#include "aprs/hdlc.h"
#include "radio/dota.h"
#include "radio/beep.h"
#include "radio/radio.h"
#include "dmr/dmrDataService.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* The two copies of dota.c */
#define SIDE(p)                                                                                                       \
	bool p##dota_start(const dota_req_t *r);                                                                     \
	void p##dota_stop(void);                                                                                      \
	void p##dota_view(dota_view_t *v);                                                                            \
	void p##dota_ptt_prompt_clear(void);                                                                          \
	bool p##dota_job_wanted(void);                                                                                \
	void p##dota_job(void *ctx);                                                                                  \
	bool p##dota_begin(dota_req_t *r, uint32_t now);                                                              \
	void p##dota_poll(const dota_io_t *io, uint32_t now);                                                         \
	bool p##dota_owns_radio(void);                                                                                \
	void p##dota_ptt(void);                                                                                       \
	void p##dota_fm_frame(const uint8_t *frame, size_t n);                                                       \
	void p##dota_dmr_rx(uint32_t src, const uint8_t *payload, int length);
SIDE(a_)
SIDE(b_)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}
void radio_req_dota(bool on) { (void)on; }

/* --- Fake transports --- */
static int side;			/* 0: the sender polls, 1: the receiver */
static bool keyed[2];
static uint32_t loss, frames_out, frames_lost, beeps[2];
static uint32_t rng = 1;
static bool lose(void)
{
	rng = rng * 1103515245u + 12345u;
	return (rng >> 8) % 100u < loss;
}
static hdlc_dec_t dec[2];
static void deliver_fm(const uint8_t *f, size_t n)
{
	frames_out++;
	if (lose()) {
		frames_lost++;
		return;
	}
	CHECK(n >= 16 && f[14] == 0x03 && f[15] == 0xF0);	/* AX.25 UI, no layer 3 */
	if (side == 0)
		b_dota_fm_frame(f, n);
	else
		a_dota_fm_frame(f, n);
}
static void play(const uint8_t *t, uint16_t n)
{
	for (uint16_t i = 0; i < n; i++) {
		size_t len = hdlc_dec_bit(&dec[side], hdlc_tone_bit(t, i));
		if (len)
			deliver_fm(dec[side].buf, len);
	}
}
static bool f_start(const uint8_t *t, uint16_t n, uint16_t baud, uint16_t m, uint16_t s, uint8_t l)
{
	(void)baud, (void)m, (void)s, (void)l;
	CHECK(keyed[side]);
	play(t, n);
	return true;
}
static bool f_more(const uint8_t *t, uint16_t n) { play(t, n); return true; }
static bool f_no(void) { return false; }
static void f_stop(void) {}
static int monitors;
static void f_monitor(bool on) { monitors += on; }
static int clicks;
static void f_click(void) { clicks++; }
static const hal_afsk_t fake_afsk = { .tx_start = f_start, .tx_busy = f_no, .tx_stop = f_stop, .tx_more = f_more,
				      .tx_queued = f_no, .monitor = f_monitor, .click = f_click };

#define ID_A 2862001u
#define ID_B 2862002u
int dmrDataServiceSendIPChain(bool group, uint32_t dst, uint32_t srcIp, uint32_t dstIp, uint8_t ttl,
			      const dmrDataChainPacket_t *p, int count)
{
	(void)srcIp, (void)dstIp, (void)ttl;
	CHECK(!group && dst == (side == 0 ? ID_B : ID_A));
	for (int i = 0; i < count; i++) {
		CHECK(p[i].protocol == DMR_IP_PROTO_UDP && p[i].delivery == DMR_IPS_UNCONFIRMED);
		CHECK(((p[i].l4[2] << 8) | p[i].l4[3]) == DOTA_PORT);
		frames_out++;
		if (lose()) {
			frames_lost++;
			continue;
		}
		if (side == 0)
			b_dota_dmr_rx(ID_A, p[i].l4 + 8, p[i].length - 8);
		else
			a_dota_dmr_rx(ID_B, p[i].l4 + 8, p[i].length - 8);
	}
	return count;
}
bool dmrDataServiceIpBusy(void) { return false; }
static uint32_t blocks;
uint32_t dmrDataServiceRxBlockCount(void) { return blocks++; }

#define FLASH_SIZE	0x200000u
static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 3, .flash = &sim_flash,
			.afsk = &fake_afsk };

static bool key_a(bool on) { keyed[0] = on; return true; }
static bool key_b(bool on) { keyed[1] = on; return true; }
static void beep_a(uint8_t b) { beeps[0] |= 1u << b; }
static void beep_b(uint8_t b) { beeps[1] |= 1u << b; }

/* --- The radio --- */
static channel_t make(const char *name, uint32_t hz, uint8_t mode)
{
	channel_t c;
	memset(&c, 0, sizeof c);
	strncpy(c.name, name, CODEPLUG_NAME_LEN);
	c.rx_hz = c.tx_hz = hz;
	c.mode = mode;
	c.bw = RADIO_BW_12K5;
	c.power = CODEPLUG_POWER_DEFAULT;
	c.color_code = 1;
	c.timeslot = 1;
	return c;
}
static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	settings_init();
	codeplug_init();
	contact_t tg = { "TG 2862", 2862, CONTACT_GROUP, 0 };
	CHECK(codeplug_set_contact(1, &tg));
	listen_list_t l = { "Mine", 1, { 1 } };
	CHECK(codeplug_set_listen_list(1, &l));
	for (int i = 0; i < 24; i++) {
		char name[17];
		snprintf(name, sizeof name, "Ch %02d", i);
		channel_t c = make(name, 145000000u + (uint32_t)i * 12500u, i % 3 == 1 ? RADIO_MODE_DMR : RADIO_MODE_FM);
		if (c.mode == RADIO_MODE_DMR) {
			c.contact = 1;
			c.listen_list = 1;
		}
		CHECK(codeplug_set_channel((uint16_t)(i + 1), &c));
	}
	zone_t z = { "Home", 16, { 0 } };
	for (int i = 0; i < 16; i++)
		z.channels[i] = (uint16_t)(i + 1);
	CHECK(zone_edit_create(&z, NULL) == ZONE_EDIT_OK);
	zone_t f = { "Far", 8, { 0 } };
	for (int i = 0; i < 8; i++)
		f.channels[i] = (uint16_t)(24 - i);
	CHECK(zone_edit_create(&f, NULL) == ZONE_EDIT_OK);
}
static int find_zone(const char *name, zone_t *out)
{
	for (uint16_t i = 0; i < codeplug_zone_count(); i++)
		if (codeplug_zone(i, out) && strcmp(out->name, name) == 0)
			return i;
	return -1;
}
static uint32_t shown(void)
{
	uint32_t h = 2166136261u;
	channel_t c;
	zone_t z;
	contact_t ct;
	for (uint16_t n = 1; n <= CODEPLUG_CHANNELS_MAX; n++)
		if (codeplug_channel(n, &c))
			for (const char *p = c.name; *p; p++)
				h = (h ^ (uint8_t)*p ^ n) * 16777619u;
	for (uint16_t i = 0; i < codeplug_zone_count(); i++)
		if (codeplug_zone(i, &z))
			h = (h ^ z.count ^ (uint32_t)z.name[0] << 8) * 16777619u;
	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++)
		if (codeplug_contact(i, &ct))
			h = (h ^ ct.id ^ i) * 16777619u;
	return h;
}

/* --- A session --- */
typedef struct {
	uint8_t method, kind;
	uint16_t zone, zone_rx, code_tx, code_rx;
	uint8_t imp_mode;
	const char *name;
	uint32_t loss;
	bool tone;			/* the receiver hears our CTCSS */
	uint32_t stop_at_ms;		/* the sender's user stops then (0: never) */
	uint32_t ptt_at_ms;		/* the receiver's user presses PTT then */
} case_t;

static dota_view_t va, vb;
static bool ptt_seen;
static uint32_t now;

static void run(const case_t *c)
{
	memset(keyed, 0, sizeof keyed);
	memset(dec, 0, sizeof dec);
	hdlc_dec_init(&dec[0]);
	hdlc_dec_init(&dec[1]);
	beeps[0] = beeps[1] = 0;
	loss = c->loss;
	frames_out = frames_lost = 0;
	dota_req_t ra = { .role = DOTA_SEND, .method = c->method, .hz = 433500000, .ctcss = 885, .cc = 1, .ts = 1,
			  .peer_id = ID_B, .power = 0, .code = c->code_tx, .call = "TA1AAA", .kind = c->kind,
			  .zone = c->zone };
	dota_req_t rb = { .role = DOTA_RECEIVE, .method = c->method, .hz = 433500000, .ctcss = 885, .cc = 1, .ts = 1,
			  .code = c->code_rx, .call = "TA1BBB-7" };
	rb.imp.mode = c->imp_mode;
	rb.imp.zone = c->zone_rx;
	strcpy(rb.imp.name, c->name ? c->name : "");
	dota_req_t got;
	CHECK(b_dota_start(&rb) && b_dota_begin(&got, now) && got.hz == 433500000);
	CHECK(a_dota_start(&ra) && a_dota_begin(&got, now));
	bool a_on = true, b_on = true;
	for (uint32_t t = 0; t < 600000u && (a_on || b_on); t += 25, now += 25) {
		if (c->stop_at_ms && t == c->stop_at_ms)
			a_dota_stop();
		if (c->ptt_at_ms && t == c->ptt_at_ms)
			b_dota_ptt();
		dota_io_t ia = { .key = key_a, .beep = beep_a, .clear = !keyed[1], .tone_ok = true, .dmr_ready = true };
		dota_io_t ib = { .key = key_b, .beep = beep_b, .clear = !keyed[0], .tone_ok = c->tone, .dmr_ready = true };
		side = 0;
		if (a_on)
			a_dota_poll(&ia, now);
		side = 1;
		if (b_on)
			b_dota_poll(&ib, now);
		if (b_dota_job_wanted())
			b_dota_job(NULL);	/* the UI posts the storage job */
		a_on = a_dota_owns_radio();
		b_on = b_dota_owns_radio();
		if (!a_on && t > 30000u && b_on && vb.phase == DOTA_P_WAIT)
			break;	/* the receiver stays armed until its user leaves */
		b_dota_view(&vb);
		ptt_seen = ptt_seen || vb.ptt_prompt;
	}
	if (b_on)
		b_dota_stop();
	for (int k = 0; k < 400 && b_dota_owns_radio(); k++, now += 25) {
		dota_io_t ib = { .key = key_b, .beep = beep_b, .clear = true, .tone_ok = false, .dmr_ready = true };
		side = 1;
		b_dota_poll(&ib, now);
	}
	while (b_dota_job_wanted())
		b_dota_job(NULL);
	a_dota_view(&va);
	b_dota_view(&vb);
}

static void test_zone_fm(uint32_t lossy)
{
	fresh();
	zone_t z;
	uint16_t home = (uint16_t)find_zone("Home", &z);
	case_t c = { .method = DOTA_FM, .kind = DOTA_KIND_ZONE, .zone = home, .code_tx = 4271, .code_rx = 4271,
		     .imp_mode = DOTA_IMP_NEW, .name = "Copy", .loss = lossy, .tone = true };
	run(&c);
	CHECK(va.result == DOTA_RES_SENT && vb.result == DOTA_RES_DONE);
	CHECK(va.ch_added == 16 && vb.ch_added == 16 && vb.zones_added == 1);
	CHECK(strcmp(vb.peer, "TA1AAA") == 0 && strcmp(va.peer, "TA1BBB") == 0);
	zone_t copy;
	CHECK(find_zone("Copy", &copy) >= 0 && copy.count == 16);
	for (int i = 0; i < copy.count; i++) {
		channel_t a, b;
		CHECK(codeplug_channel(z.channels[i], &a) && codeplug_channel(copy.channels[i], &b));
		CHECK(strcmp(a.name, b.name) == 0 && a.rx_hz == b.rx_hz && a.mode == b.mode && b.rx_only);
		CHECK(copy.channels[i] != z.channels[i]);
		if (b.mode == RADIO_MODE_DMR)
			CHECK(b.contact == 1 && b.listen_list == 1);	/* matched: the radio's own contact and list */
	}
	CHECK(monitors > 0);	/* the FM sender's speaker followed its own tones */
	CHECK((beeps[0] >> BEEP_DOTA_MARK) & 1u && (beeps[1] >> BEEP_DOTA_MARK) & 1u);
	printf("  FM zone of 16, %u %% loss: done, %u frames, %u lost\n", lossy, frames_out, frames_lost);
}

static void test_clone_dmr(uint32_t lossy)
{
	fresh();
	case_t c = { .method = DOTA_DMR, .kind = DOTA_KIND_CLONE, .code_tx = 77, .code_rx = 77, .loss = lossy,
		     .tone = true };
	uint16_t zones0 = codeplug_zone_count();
	run(&c);
	CHECK(va.result == DOTA_RES_SENT && vb.result == DOTA_RES_DONE);
	zone_t z;
	int z0 = find_zone("Home 2", &z);
	CHECK(z0 >= 0 && z.count == 16);
	CHECK(find_zone("Far 2", &z) >= 0 && z.count == 8);
	/* every zone the radio stores came again (with the "All channels" zone the first edit stored, when it is one) */
	CHECK(vb.ch_added == 24 && vb.zones_added == zones0 && codeplug_zone_count() == 2u * zones0);
	CHECK(clicks > 0 && ((beeps[1] >> BEEP_DOTA_DATA) & 1u));	/* the DMR sounds: clicks keyed, crackle */
	printf("  DMR clone (24 channels, %u zones), %u %% loss: done, %u datagrams, %u lost\n", zones0, lossy,
	       frames_out, frames_lost);
}

static void test_failures(void)
{
	/* A wrong code: no answer, nothing written */
	fresh();
	uint32_t before = shown();
	zone_t z;
	case_t c = { .method = DOTA_FM, .kind = DOTA_KIND_ZONE, .zone = (uint16_t)find_zone("Home", &z), .code_tx = 1111,
		     .code_rx = 2222, .imp_mode = DOTA_IMP_NEW, .name = "X", .tone = true };
	run(&c);
	CHECK(va.result == DOTA_RES_NO_ANSWER && vb.result != DOTA_RES_DONE && shown() == before);
	/* No CTCSS heard: the receiver hears nothing */
	c.code_rx = 1111;
	c.tone = false;
	run(&c);
	CHECK(va.result == DOTA_RES_NO_ANSWER && shown() == before);
	/* The sender stops part way: the receiver changes nothing (contacts it added go again) */
	fresh();
	before = shown();
	case_t s = { .method = DOTA_DMR, .kind = DOTA_KIND_CLONE, .code_tx = 9, .code_rx = 9, .tone = true,
		     .stop_at_ms = 1000 };
	run(&s);
	CHECK(va.result == DOTA_RES_STOPPED && vb.result != DOTA_RES_DONE);
	CHECK(shown() == before);
	/* The receiver's PTT: the prompt shows, the transfer goes on */
	fresh();
	case_t p = { .method = DOTA_FM, .kind = DOTA_KIND_ZONE, .zone = (uint16_t)find_zone("Far", &z), .code_tx = 5,
		     .code_rx = 5, .imp_mode = DOTA_IMP_ADD, .tone = true, .ptt_at_ms = 500 };
	p.zone_rx = (uint16_t)find_zone("Home", &z);
	ptt_seen = false;
	run(&p);
	CHECK(va.result == DOTA_RES_SENT && vb.result == DOTA_RES_DONE);
	zone_t home;
	CHECK(find_zone("Home", &home) >= 0 && home.count == 16 + 8);	/* added to the end of "Home" */
	CHECK(ptt_seen);	/* the prompt was up while the transfer went on */
}

int main(void)
{
	now = 1000;
	test_zone_fm(0);
	test_zone_fm(10);
	test_clone_dmr(0);
	test_clone_dmr(10);
	test_failures();
	printf("dota session: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
