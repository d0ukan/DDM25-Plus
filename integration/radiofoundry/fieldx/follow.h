/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM25 Plus "find my friend" (CONFIG_NAV_FOLLOW, docs/plus.md): Go-To follows one APRS station. Its callsign and
 * last position are kept here (one target, 24 B of RAM); every second the UI looks the station up in the APRS
 * engine's heard list (upstream's, no copy) and takes a newer position when one was heard. The position stays when
 * the station drops out of the list; its age keeps counting, and from FOLLOW_STALE_MS it is marked stale. Started
 * from Tools > Map's station pick (5) or Tools > APRS > Stations heard (OK); Back on Go-To or another target there
 * ends it. Installed as core/ui/follow.{c,h}; pure (the host tests run it as is). */
#ifndef PLUS_FOLLOW_H
#define PLUS_FOLLOW_H

#include <stdbool.h>
#include <stdint.h>

#define FOLLOW_CALL_LEN		10u		/* "TA1ABC-15" and its NUL (AX25_CALL_LEN + 4) */
#define FOLLOW_STALE_MS		600000u		/* 10 minutes */
#define FOLLOW_POLL_MS		1000u
#define FOLLOW_GOTO_ARG		0xFFFFu		/* Go-To's frame arg: follow the station set */

typedef struct {
	char call[FOLLOW_CALL_LEN];	/* "" = not following */
	bool has_pos;
	int32_t lat, lon;		/* 1e-7 degree, the last position heard */
	uint32_t heard_ms;		/* when the station was last heard (the engine's time) */
} follow_t;

/* Start following call (cut to fit), nothing known of it yet */
void follow_set(follow_t *f, const char *call);
void follow_clear(follow_t *f);
bool follow_on(const follow_t *f);
/* A station from the heard list: when it is ours and newer than what we have (or the first), take it. True when the
 * position or the time changed */
bool follow_update(follow_t *f, const char *call, bool has_pos, int32_t lat, int32_t lon, uint32_t heard_ms);
/* How long ago it was last heard (0 when never), and whether that is FOLLOW_STALE_MS or more */
uint32_t follow_age_ms(const follow_t *f, uint32_t now_ms);
bool follow_stale(const follow_t *f, uint32_t now_ms);

/* --- The UI task (ui_goto.c) --- */
/* Follow call: Go-To opened on it (the map's pick, the heard list) */
void ui_follow_start(const char *call);
/* Every UI tick: the station looked up once a second; true when Go-To shows it and should be drawn again */
bool ui_follow_tick(uint8_t scr, uint32_t now_ms);

#endif
