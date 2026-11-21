/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus DOTA Transfer: the records, format version 1 (docs/dota.md 9). A DATA frame's payload is whole records
 * of one section, back to back; each decoder says how many bytes its record took (0: refused). No radio, no codeplug
 * access: the sender's source reads the codeplug and calls the encoders, the receiver's import calls the decoders.
 * Never encoded: the per-channel DMR ID, the P25 unit ID, the APRS configuration, the fixed location (identities and
 * the sender's own); a decoded channel has them 0 ("the radio's own"). Installed as core/radio/dota_rec.h. */
#ifndef DOTA_REC_H
#define DOTA_REC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "codeplug/codeplug.h"

/* A channel with its DMR references by value: the contact by ID and type, the RX group list by its index in the
 * transfer (the lists section) */
enum { DOTA_CT_NONE, DOTA_CT_GROUP, DOTA_CT_PRIVATE, DOTA_CT_ALL };
#define DOTA_LIST_NONE	0u
#define DOTA_LIST_ANY	0xFFu	/* hear every talkgroup (CODEPLUG_LIST_ANY) */
typedef struct {
	channel_t ch;
	uint8_t ct_type;	/* DOTA_CT_* */
	uint32_t ct_id;
	uint8_t list;		/* DOTA_LIST_NONE, DOTA_LIST_ANY, else the transfer's list index + 1 */
} dota_ch_t;

#define DOTA_CH_MAX	48u	/* the longest channel record */
/* power_count: this radio's power levels (board.power_count), for the power class */
size_t dota_rec_put_channel(const dota_ch_t *c, uint8_t power_count, uint8_t *out, size_t max);
size_t dota_rec_get_channel(const uint8_t *in, size_t n, uint8_t power_count, dota_ch_t *c);

/* A contact: ID, type (contact_type_t), the timeslot override, the name */
#define DOTA_CT_MAX	22u
size_t dota_rec_put_contact(const contact_t *c, uint8_t *out, size_t max);
size_t dota_rec_get_contact(const uint8_t *in, size_t n, contact_t *c);

/* An RX group list: the name and its members by ID and call type (matched against the receiver's contacts) */
typedef struct {
	char name[CODEPLUG_NAME_LEN + 1];
	uint8_t count;
	uint32_t id[CODEPLUG_LIST_LEN];
	uint32_t private_bits;	/* bit i: member i is a private call (else a talkgroup) */
} dota_list_t;
#define DOTA_LIST_MAX	(1u + CODEPLUG_NAME_LEN + 1u + 4u + 3u * CODEPLUG_LIST_LEN)	/* 118 */
size_t dota_rec_put_list(const dota_list_t *l, uint8_t *out, size_t max);
size_t dota_rec_get_list(const uint8_t *in, size_t n, dota_list_t *l);

/* A zone part: members as runs of the transfer's channel indices. A zone goes in parts of DOTA_ZONE_RUNS runs, each
 * standing alone (its members' offset in the zone and the zone's size in each); the name in part 0 */
#define DOTA_ZONE_RUNS	32u
typedef struct {
	uint8_t zone;		/* the zone's index in the transfer */
	uint8_t part;
	uint8_t total;		/* the zone's members, 1..80 */
	uint8_t offset;		/* the first member of this part */
	char name[CODEPLUG_NAME_LEN + 1];	/* part 0 only */
	uint8_t runs;
	uint16_t start[DOTA_ZONE_RUNS];
	uint8_t len[DOTA_ZONE_RUNS];
} dota_zone_t;
#define DOTA_ZONE_MAX	(5u + 1u + CODEPLUG_NAME_LEN + 3u * DOTA_ZONE_RUNS)	/* 118 */
size_t dota_rec_put_zone(const dota_zone_t *z, uint8_t *out, size_t max);
size_t dota_rec_get_zone(const uint8_t *in, size_t n, dota_zone_t *z);

/* The power class of a level and back (0 default, 1 low, 2 mid, 3 high) */
uint8_t dota_power_class(uint8_t power, uint8_t power_count);
uint8_t dota_power_level(uint8_t cls, uint8_t power_count);

#endif
