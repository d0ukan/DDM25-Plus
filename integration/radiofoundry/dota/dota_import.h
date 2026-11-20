/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: the receiver's import (docs/dota.md 10). The capacity check at HELLO, each DATA frame
 * staged as it comes (contacts and RX group lists matched or added, channels and zones staged without their in-use
 * bits), one commit at the end, and the undo of a transfer that didn't finish. Everything but dota_imp_begin runs
 * where codeplug writes run (the start-up task's storage jobs); its working state is static (the caller's), as that
 * task's stack is small. Installed as core/radio/dota_import.h. */
#ifndef DOTA_IMPORT_H
#define DOTA_IMPORT_H

#include <stdbool.h>
#include <stdint.h>
#include "codeplug/codeplug.h"
#include "radio/dota_proto.h"
#include "radio/dota_rec.h"

/* What is sent (the manifest's kind) */
enum { DOTA_KIND_CHANNEL, DOTA_KIND_SELECTED, DOTA_KIND_ZONE, DOTA_KIND_CLONE };
/* Where one channel, selected channels or one zone go */
enum { DOTA_IMP_NEW, DOTA_IMP_ADD };

typedef struct {
	uint8_t mode;				/* DOTA_IMP_NEW: a new zone named name; DOTA_IMP_ADD: the end of zone */
	bool tx_ok;				/* imported channels may transmit (else every one is receive only) */
	uint16_t zone;
	char name[CODEPLUG_NAME_LEN + 1];
} dota_imp_cfg_t;

typedef struct {
	dota_imp_cfg_t cfg;
	uint8_t kind;
	uint8_t zones;				/* DOTA Clone: zones in the transfer */
	uint16_t channels;			/* channels in the transfer */
	uint8_t power_count;
	bool failed;				/* a flash write failed: the transfer fails */
	uint8_t ct_free[CODEPLUG_CONTACTS_MAX / 8u];	/* contact slots free at the start: the undo deletes those used */
	uint8_t list_free[(CODEPLUG_LISTS_MAX + 7u) / 8u];
	uint8_t staged[CODEPLUG_CHANNELS_MAX / 8u];	/* the transfer's channel k is staged (rank k) */
	uint8_t zstaged[(CODEPLUG_ZONES_MAX + 7u) / 8u];	/* the transfer's zone z has its staged record */
	uint8_t list_map[CODEPLUG_LISTS_MAX];	/* the transfer's list i: its slot here (0: none) */
	uint16_t added, skipped;		/* channels staged, left out (not in this radio's bands, refused) */
	uint16_t ct_added, ct_matched;
	uint8_t lists_added, lists_matched, zones_added;
	uint32_t cache_id;			/* the last contact looked up */
	uint8_t cache_type;
	uint16_t cache_slot;
	union {
		dota_ch_t ch;
		contact_t ct;
		dota_list_t list;
		dota_zone_t zp;
		listen_list_t ll;
		uint16_t numbers[CODEPLUG_ZONE_MAX];	/* the commit: a zone's channel numbers */
	} rec;					/* the record being taken */
	listen_list_t have;			/* a list here, compared */
	zone_t z;				/* a zone being built */
} dota_imp_t;

/* HELLO: is there room for this transfer here? DOTA_R_OK or the refusal; free_ch, free_zones and room (the room left in
 * the zone channels are added to) for the answer. Takes the snapshots the undo needs. Any task (reads only) */
uint8_t dota_imp_begin(dota_imp_t *im, const dota_imp_cfg_t *cfg, const dota_manifest_t *m, uint8_t power_count,
		       uint16_t *free_ch, uint8_t *free_zones, uint8_t *room);
/* One DATA frame's records. False when a write failed (im->failed) */
bool dota_imp_frame(dota_imp_t *im, uint8_t sec, uint16_t first, uint8_t count, const uint8_t *p, uint8_t len);
/* Every frame is in and checks: make it all visible. False when a write failed (what is written stays: a flash fault) */
bool dota_imp_commit(dota_imp_t *im);
/* The transfer ended without a commit: the contacts and RX group lists it added go, staged P25 tags go */
void dota_imp_undo(dota_imp_t *im);

/* The codeplug's side (codeplug_dota.inc, included in codeplug.c) */
bool codeplug_stage_channel(uint16_t number, const channel_t *ch);
uint16_t codeplug_free_nth(uint16_t rank);	/* 1-based channel number of the rank-th free slot, 0 none */
uint16_t codeplug_free_channels(void);
bool codeplug_commit_staged(const uint8_t *ranks, uint16_t count);
uint8_t codeplug_free_zone_slots(void);
bool codeplug_stage_zone(uint8_t rank, const zone_t *z);
bool codeplug_staged_zone(uint8_t rank, zone_t *z);

#endif
