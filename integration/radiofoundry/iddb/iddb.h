/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR ID database (CONFIG_ID_DB, docs/plus.md): callsign and first name for a DMR ID, from a table in
 * program flash that tools/iddb.py makes from RadioID.net's user list at build time (radio/iddb_data.c; a few
 * fictional sample IDs when the build has no download). Read only: nothing is written anywhere, no RAM besides the
 * caller's result. The radio task names a caller with it when no codeplug contact has the ID (radio_sm.c
 * find_name), before upstream's CPS-uploaded database in the data flash.
 *
 * Layout (tools/iddb.py): records sorted by ID in blocks of IDDB_BLOCK; an index of each block's first ID and byte
 * offset is binary searched, then the block is walked. A record: LEB128 distance from the previous ID (0 for a
 * block's first), callsign base-37 in 4 bytes LE (6 characters, ' ' padded on the right), name number 2 bytes LE. */
#ifndef PLUS_IDDB_H
#define PLUS_IDDB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IDDB_BLOCK	64	// records a block (tools/iddb.py BLOCK)
#define IDDB_CALL_MAX	6	// callsign characters
#define IDDB_NAME_MAX	10	// name characters (tools/iddb.py NAME_MAX)

typedef struct {
	uint32_t count;		// IDs
	uint32_t blocks;	// index entries
	uint32_t record_bytes;	// iddb_records[] length
	uint8_t sample;		// 1: the committed fictional sample, not RadioID.net data
	char date[11];		// "2026-11-18" (the download's date), "sample"
	char source[12];	// "RadioID.net", "sample"
} iddb_info_t;

typedef struct {
	uint32_t first_id;	// the block's first ID
	uint32_t offset;	// its first record in iddb_records[]
} iddb_block_t;

typedef struct {
	char call[IDDB_CALL_MAX + 1];	// "TA1ABC"
	const char *name;		// "Ahmet" (in flash), "" if none
} iddb_entry_t;

// The generated table (radio/iddb_data.c)
extern const iddb_info_t iddb_info;
extern const iddb_block_t iddb_index[];
extern const uint8_t iddb_records[];
extern const uint16_t iddb_name_off[];
extern const char iddb_names[];

// The entry of a DMR ID, false if the ID is not in the database. log2(blocks) index steps, then IDDB_BLOCK
// records at most
bool iddb_lookup(uint32_t id, iddb_entry_t *out);
// "TA1ABC Ahmet" (the callsign alone without a name) into out, cut to len - 1 characters; false (out "") if the
// ID is not in the database
bool iddb_text(uint32_t id, char *out, size_t len);
// A base-37 callsign as text (out IDDB_CALL_MAX + 1 bytes)
void iddb_call(uint32_t packed, char *out);

#endif
