/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus DMR ID database (radio/iddb.c and a table tools/iddb.py made):
 *  test_iddb EXPECT   a table made from tools/test_classic.py's synthetic CSV: every ID in EXPECT ("id\tcall\tname"
 *                     lines, "-\tid" for one that must be missing) found with its callsign and folded name, the
 *                     neighbours of each and the missing ones not; boundaries 1 and 0xFFFFFF, IDs past 24 bits,
 *                     the first row of a duplicate, callsigns with digits, LEB128 gaps of 1-3 bytes, the text
 *                     ("TA1ABC Ahmet") cut to every length, the 16-character name the radio keeps;
 *  test_iddb          the table this prepare made (RadioID.net download or the sample): a walk of every record
 *                     against the lookup, IDs rising, every name in the name table. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio/iddb.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct {
	uint32_t id;
	char call[8];
	char name[16];
} want_t;

static want_t *want;
static size_t nwant;
static uint32_t *missing;
static size_t nmissing;

static int by_id(const void *a, const void *b)
{
	uint32_t x = ((const want_t *)a)->id, y = ((const want_t *)b)->id;
	return x < y ? -1 : x > y;
}

static bool wanted(uint32_t id)
{
	want_t key = { .id = id };
	return bsearch(&key, want, nwant, sizeof *want, by_id) != NULL;
}

static void load(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		perror(path);
		exit(2);
	}
	char line[128];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *tab = strchr(line, '\t');
		if (tab == NULL)
			continue;
		*tab = 0;
		if (strcmp(line, "-") == 0) {
			missing = realloc(missing, (nmissing + 1) * sizeof *missing);
			missing[nmissing++] = (uint32_t)strtoul(tab + 1, NULL, 10);
			continue;
		}
		want = realloc(want, (nwant + 1) * sizeof *want);
		want_t *w = &want[nwant++];
		memset(w, 0, sizeof *w);
		w->id = (uint32_t)strtoul(line, NULL, 10);
		char *tab2 = strchr(tab + 1, '\t');
		if (tab2 != NULL) {
			*tab2 = 0;
			snprintf(w->name, sizeof w->name, "%s", tab2 + 1);
		}
		snprintf(w->call, sizeof w->call, "%s", tab + 1);
	}
	fclose(f);
	qsort(want, nwant, sizeof *want, by_id);
}

/* Every record in table order, decoded here independently of iddb_lookup's search */
static uint32_t walk(void)
{
	uint32_t n = 0, prev = 0;
	for (uint32_t b = 0; b < iddb_info.blocks; b++) {
		uint32_t off = iddb_index[b].offset;
		uint32_t end = b + 1 < iddb_info.blocks ? iddb_index[b + 1].offset : iddb_info.record_bytes;
		uint32_t id = iddb_index[b].first_id;
		CHECK(b == 0 || id > prev);
		bool first = true;
		while (off < end) {
			uint32_t d = 0;
			for (unsigned s = 0;; s += 7) {
				uint8_t c = iddb_records[off++];
				d |= (uint32_t)(c & 0x7F) << s;
				if (!(c & 0x80))
					break;
			}
			CHECK(first ? d == 0 : d > 0);
			first = false;
			id += d;
			CHECK(id >= 1 && id <= 0xFFFFFFu);
			CHECK(n == 0 || id > prev);
			uint32_t call = iddb_records[off] | iddb_records[off + 1] << 8 | iddb_records[off + 2] << 16 |
					(uint32_t)iddb_records[off + 3] << 24;
			uint16_t name = (uint16_t)(iddb_records[off + 4] | iddb_records[off + 5] << 8);
			off += 6;
			char c[IDDB_CALL_MAX + 1];
			iddb_call(call, c);
			iddb_entry_t e;
			CHECK(iddb_lookup(id, &e));
			CHECK(strcmp(e.call, c) == 0);
			CHECK(e.name == iddb_names + iddb_name_off[name]);
			CHECK(strlen(e.name) <= IDDB_NAME_MAX);
			CHECK(c[0] != 0);
			if (n > 0 && id - 1 != prev)
				CHECK(!iddb_lookup(id - 1, &e));
			prev = id;
			n++;
		}
		CHECK(off == end);
		CHECK(n <= (b + 1) * IDDB_BLOCK);
	}
	return n;
}

static void text_cuts(uint32_t id, const char *full)
{
	char buf[40];
	size_t cl = strcspn(full, " ");
	for (size_t len = 1; len < sizeof buf; len++) {
		memset(buf, 'x', sizeof buf);
		CHECK(iddb_text(id, buf, len));
		CHECK(strlen(buf) < len);
		/* the callsign cut, or the callsign alone, or "CALL N..." cut: never a trailing space */
		size_t n = strlen(buf);
		CHECK(strncmp(buf, full, n) == 0);
		CHECK(n == 0 || buf[n - 1] != ' ');
		if (len <= cl)
			CHECK(n == len - 1);
		else if (len == cl + 1 || len == cl + 2)
			CHECK(n == cl);
		else
			CHECK(n == (strlen(full) < len - 1 ? strlen(full) : len - 1));
	}
	CHECK(!iddb_text(id, buf, 0));
}

static void synthetic(const char *path)
{
	load(path);
	CHECK(nwant > 200);
	CHECK(iddb_info.count == nwant);
	CHECK(iddb_info.sample == 0);
	CHECK(strcmp(iddb_info.date, "2026-01-02") == 0);
	CHECK(strcmp(iddb_info.source, "RadioID.net") == 0);
	CHECK(walk() == nwant);
	CHECK(iddb_info.blocks == (nwant + IDDB_BLOCK - 1) / IDDB_BLOCK);
	for (size_t i = 0; i < nwant; i++) {
		const want_t *w = &want[i];
		iddb_entry_t e;
		memset(&e, 0, sizeof e);
		bool ok = iddb_lookup(w->id, &e);
		CHECK(ok);
		if (!ok) {
			printf("  id %u missing\n", w->id);
			continue;
		}
		if (strcmp(e.call, w->call) != 0 || strcmp(e.name, w->name) != 0) {
			failures++;
			printf("FAIL id %u: got \"%s\" \"%s\", want \"%s\" \"%s\"\n", w->id, e.call, e.name, w->call, w->name);
		}
		char full[32], got[32];
		snprintf(full, sizeof full, w->name[0] ? "%s %s" : "%s", w->call, w->name);
		CHECK(iddb_text(w->id, got, sizeof got));
		CHECK(strcmp(got, full) == 0);
		text_cuts(w->id, full);
		for (int d = -1; d <= 1; d += 2) {
			uint32_t n = w->id + (uint32_t)d;
			if (!wanted(n)) {
				CHECK(!iddb_lookup(n, &e));
				strcpy(got, "junk");
				CHECK(!iddb_text(n, got, sizeof got));
				CHECK(got[0] == 0);
			}
		}
	}
	for (size_t i = 0; i < nmissing; i++) {
		iddb_entry_t e;
		CHECK(!wanted(missing[i]));
		CHECK(!iddb_lookup(missing[i], &e));
	}
	iddb_entry_t e;
	CHECK(!iddb_lookup(0, &e));
	CHECK(!iddb_lookup(0x1000000u, &e));
	CHECK(!iddb_lookup(0xFFFFFFFFu, &e));
	/* boundaries are in the fixture */
	CHECK(wanted(1) && wanted(0xFFFFFFu));
	CHECK(iddb_lookup(1, &e) && iddb_lookup(0xFFFFFFu, &e));
	/* the radio keeps 16 characters (CODEPLUG_NAME_LEN + 1 bytes, radio_sm.c find_name) */
	char name17[17];
	CHECK(iddb_text(2861000u, name17, sizeof name17));
	CHECK(strcmp(name17, "TA1ABC Abdurrahm") == 0);
	/* base-37 corners */
	char c[IDDB_CALL_MAX + 1];
	iddb_call(0, c);
	CHECK(c[0] == 0);
	iddb_call(2565726408u, c);	/* 37^6 - 1 */
	CHECK(strcmp(c, "999999") == 0);
	iddb_call(1u * 37u * 37u * 37u * 37u * 37u, c);
	CHECK(strcmp(c, "A") == 0);
	iddb_call(27u, c);
	CHECK(strcmp(c, "     0") == 0);	/* only trailing pads are trimmed (the generator never leads with one) */
	printf("test_iddb: %zu IDs, %zu missing checked, %u blocks, %u record bytes\n", nwant, nmissing,
	       iddb_info.blocks, iddb_info.record_bytes);
}

static void shipped(void)
{
	uint32_t n = walk();
	CHECK(n == iddb_info.count);
	CHECK(n > 0);
	if (iddb_info.sample) {
		char t[17];
		CHECK(iddb_text(9990002u, t, sizeof t) && strcmp(t, "TA0AAB Ornek") == 0);
		CHECK(iddb_text(9990004u, t, sizeof t) && strcmp(t, "TA0SIL Sukru") == 0);
		CHECK(iddb_text(9990005u, t, sizeof t) && strcmp(t, "TA0TST Igdir") == 0);
		CHECK(!iddb_text(9990007u, t, sizeof t));
	}
	printf("test_iddb: shipped table %u IDs (%s, %s), %u blocks, %u record bytes\n", n,
	       iddb_info.sample ? "sample" : iddb_info.source, iddb_info.date, iddb_info.blocks, iddb_info.record_bytes);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		synthetic(argv[1]);
	else
		shipped();
	if (failures) {
		printf("test_iddb: %d failures\n", failures);
		return 1;
	}
	printf("test_iddb: ok\n");
	return 0;
}
