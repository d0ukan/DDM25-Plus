/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus QSO log (integration/radiofoundry/qso): the marks and frequencies beside Recent through its moves (a new
 * station on top, the same one again moving up with its mark, the oldest dropped when full), the * key's mark and the
 * user's choice staying, the automatic mark (our TX on the call's frequency within 60 s, before or after; a DMR call we
 * made; not on another frequency, not later than that), and the debug link's export byte for byte, including its
 * refusal while a change is under way. The Recent list here is a copy of ui_app.c's recent_add with the same hooks
 * (tools/plus_misc.py prepare_qso). Built with -DCONFIG_QSO_LOG=1 -DCONFIG_CALL_LOG=1 -DCONFIG_P25_EXTRAS=1. */
#include <stdio.h>
#include <string.h>
#include "ui/qso_log.h"
#include "ui/ui_nav.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static ui_recent_t recent[UI_RECENT_MAX];
static uint8_t recent_n;

uint8_t ui_recent_count(void) { return recent_n; }
const ui_recent_t *ui_recent_get(uint8_t i) { return i < recent_n ? &recent[i] : NULL; }

/* ui_app.c's recent_add (CONFIG_CALL_LOG) with prepare_qso's two hooks */
static void recent_add(const ui_recent_t *r)
{
	uint8_t i;
	for (i = 0; i < recent_n; i++)
		if (recent[i].src == r->src && recent[i].dst == r->dst && recent[i].out == r->out &&
		    recent[i].mode == r->mode && recent[i].nac == r->nac && (r->mode == 0 || strcmp(recent[i].name, r->name) == 0))
			break;
	bool qso_dup = i < recent_n;
	if (i == recent_n && recent_n < UI_RECENT_MAX)
		recent_n++;
	if (i >= recent_n)
		i = recent_n - 1;
	memmove(&recent[1], &recent[0], i * sizeof recent[0]);
	recent[0] = *r;
	qso_log_shift(i, qso_dup);
}

static radio_view_t view;
static uint32_t now = 100000;

/* One UI tick: the changes between begin and end, as ui_app_tick */
static void heard(uint32_t src, uint32_t dst, uint32_t hz, uint8_t mode, const char *name)
{
	ui_recent_t r;
	memset(&r, 0, sizeof r);
	r.src = src;
	r.dst = dst;
	r.group = true;
	r.mode = mode;
	r.ms = now;
	if (name)
		strcpy(r.name, name);
	view.ch.rx_hz = hz;
	view.state = RS_RX;
	qso_log_begin();
	recent_add(&r);
	qso_log_end(&view, now);
}

static void made(uint32_t dst, uint32_t hz)
{
	ui_recent_t r;
	memset(&r, 0, sizeof r);
	r.src = 2860001;
	r.dst = dst;
	r.group = true;
	r.out = true;
	r.ms = now;
	view.ch.rx_hz = hz;
	view.state = RS_TX;
	qso_log_begin();
	recent_add(&r);
	qso_log_end(&view, now);
	view.state = RS_IDLE;
}

static void tick(uint32_t hz, radio_state_t st)
{
	view.ch.rx_hz = hz;
	view.state = st;
	qso_log_begin();
	qso_log_end(&view, now);
}

static int find(uint32_t src)
{
	for (uint8_t i = 0; i < recent_n; i++)
		if (recent[i].src == src && !recent[i].out)
			return i;
	return -1;
}

static bool marked(uint32_t src)
{
	int i = find(src);
	return i >= 0 && qso_log_marked((uint8_t)i);
}

static void reset(void)
{
	recent_n = 0;
	memset(recent, 0, sizeof recent);
	qso_log_reset();
	memset(&view, 0, sizeof view);
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int main(void)
{
	/* Moves: marks and frequencies follow their entries */
	reset();
	heard(1001, 91, 439100000, 0, "TA1AAA Ali");
	now += 1000;
	heard(1002, 91, 145500000, 0, NULL);
	now += 1000;
	heard(1003, 101, 433500000, RADIO_MODE_P25, NULL);
	CHECK(recent_n == 3 && find(1001) == 2 && qso_log_hz(2) == 439100000 && qso_log_hz(0) == 433500000);
	CHECK(!qso_log_marked(0) && !qso_log_marked(1) && !qso_log_marked(2));
	CHECK(qso_log_toggle(2) && marked(1001) && qso_log_user(2));		/* * on 1001 */
	CHECK(!qso_log_toggle(5));						/* past the list: nothing */
	now += 1000;
	heard(1004, 91, 439100000, 0, NULL);
	CHECK(find(1001) == 3 && marked(1001) && !marked(1004) && qso_log_hz(3) == 439100000);
	now += 1000;
	heard(1001, 91, 446006250, 0, "TA1AAA Ali");	/* the same station again: up with its mark, its new frequency */
	CHECK(recent_n == 4 && find(1001) == 0 && marked(1001) && qso_log_user(0) && qso_log_hz(0) == 446006250);
	CHECK(!marked(1004) && !marked(1002) && !marked(1003));
	/* Full: the oldest goes, a new station starts unmarked even if the dropped one had a mark */
	reset();
	for (uint32_t k = 0; k < UI_RECENT_MAX; k++) {
		now += 70000;
		heard(2000 + k, 9, 145500000, 0, NULL);
	}
	CHECK(qso_log_toggle(UI_RECENT_MAX - 1) && marked(2000));	/* the oldest */
	now += 70000;
	heard(3000, 9, 145500000, 0, NULL);
	CHECK(recent_n == UI_RECENT_MAX && find(2000) < 0 && !qso_log_marked(0) && !marked(3000));
	for (uint8_t i = 0; i < UI_RECENT_MAX; i++)
		CHECK(!qso_log_marked(i));

	/* Automatic: our TX on the call's frequency within 60 s after it */
	reset();
	heard(5001, 91, 439100000, 0, NULL);
	now += 59000;
	tick(439100000, RS_TX);
	CHECK(marked(5001));
	/* ... and before it (a call answering ours) */
	now += 1000;
	tick(439100000, RS_IDLE);
	now += 30000;
	heard(5002, 91, 439100000, 0, NULL);
	CHECK(marked(5002));
	/* ... not on another frequency, not 61 s later */
	now += 100000;
	heard(5003, 91, 145500000, 0, NULL);
	tick(145600000, RS_TX);
	tick(145600000, RS_IDLE);
	CHECK(!marked(5003));
	now += 61000;
	tick(145500000, RS_TX);
	tick(145500000, RS_IDLE);
	CHECK(!marked(5003));
	/* A DMR call we made (its own Recent entry) marks the heard ones around it on its frequency */
	now += 200000;
	heard(5004, 91, 438500000, 0, NULL);
	now += 10000;
	made(91, 438500000);
	CHECK(marked(5004) && !qso_log_marked(0));	/* our own entry is never marked */
	/* The user's choice stays: unmarked by *, the next TX does not mark it again */
	int i5004 = find(5004);
	CHECK(i5004 >= 0 && !qso_log_toggle((uint8_t)i5004) && !marked(5004));
	tick(438500000, RS_TX);
	CHECK(!marked(5004));

	/* The export, byte for byte */
	reset();
	now = 1000000;
	heard(3101234, 91, 439100000, 0, "TA1ABC Ahmet");
	recent[0].to_us = false;
	now += 2000;
	heard(77285, 101, 433500000, RADIO_MODE_P25, NULL);
	qso_log_begin();
	recent[0].nac = 0x659;
	recent[0].enc = true;
	recent[0].emerg = true;
	recent[0].group = false;	/* unit to unit */
	qso_log_end(&view, now);
	CHECK(qso_log_toggle(1));
	now += 500;
	uint8_t out[1024];
	size_t len = qso_log_export(out, sizeof out, now);
	CHECK(len == QSO_EXPORT_HEAD + 2u * QSO_EXPORT_ENTRY && QSO_EXPORT_ENTRY == 36u);
	CHECK(out[0] == 1 && out[1] == 2 && le32(out + 2) == now && le32(out + 6) == 60);
	const uint8_t *e = out + QSO_EXPORT_HEAD;
	CHECK(le32(e) == 500 && e[4] == RADIO_MODE_P25 && e[5] == (QSO_F_ENC | QSO_F_EMERG) && le16(e + 6) == 0x659);
	CHECK(le32(e + 8) == 77285 && le32(e + 12) == 101 && le32(e + 16) == 433500000 && e[20] == 0);
	e += QSO_EXPORT_ENTRY;
	CHECK(le32(e) == 2500 && e[4] == 0 && e[5] == (QSO_F_GROUP | QSO_F_QSO | QSO_F_USER) && le16(e + 6) == 0);
	CHECK(le32(e + 8) == 3101234 && le32(e + 12) == 91 && le32(e + 16) == 439100000);
	CHECK(memcmp(e + 20, "TA1ABC Ahmet\0\0\0\0", 16) == 0);
	/* A 16 character name fills its field without a terminator; an empty list is the header alone */
	qso_log_begin();
	strcpy(recent[1].name, "ABCDEFGHIJKLMNOP");
	qso_log_end(&view, now);
	len = qso_log_export(out, sizeof out, now);
	CHECK(len == 82 && memcmp(out + QSO_EXPORT_HEAD + QSO_EXPORT_ENTRY + 20, "ABCDEFGHIJKLMNOP", 16) == 0);
	/* While Recent changes (between begin and end) it refuses, and after it answers again */
	qso_log_begin();
	CHECK(qso_log_export(out, sizeof out, now) == 0);
	qso_log_end(&view, now);
	CHECK(qso_log_export(out, sizeof out, now) == len);
	/* Too small a buffer: refused, nothing written past it */
	CHECK(qso_log_export(out, len - 1, now) == 0);
	/* The full list fits a debug link reply (1024 bytes with the status) */
	reset();
	for (uint32_t k = 0; k < UI_RECENT_MAX; k++)
		heard(4000 + k, 9, 145500000, 0, "NAME");
	len = qso_log_export(out, 1023, now);
	CHECK(len == QSO_EXPORT_HEAD + UI_RECENT_MAX * QSO_EXPORT_ENTRY && len + 1 <= 1024);
	reset();
	CHECK(qso_log_export(out, sizeof out, now) == QSO_EXPORT_HEAD && out[1] == 0);

	printf("QSO log: %d failures\n", failures);
	return failures != 0;
}
