/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus map navigation (docs/map.md "APRS stations, auto repeater, track"): the portable part, no RTOS, no heap,
 * no float (the host tests run it as is). Installed as core/ui/mapnav.{c,h}.
 *   - APRS stations on the map: upstream's heard list (aprs_station_t, APRS_STATIONS entries, kept by the APRS
 *     engine with the position, symbol and comment; the Plus build adds the altitude): no copy. The map's pick
 *     walks it nearest the map's centre first, a station named by a hash of its callsign (the list reorders as
 *     stations are heard, an index would move).
 *   - Track (CONFIG_MAP_TRACK): the last TRACK_POINTS GPS positions, a point every TRACK_EVERY_MS once we moved
 *     TRACK_MIN_M from the last one, as 16-bit offsets of 0.001 degree from the map data's origin (the map format;
 *     about 100 m); the oldest is dropped when full. The first point after a clear is kept exactly for "Back to start".
 *   - Auto repeater (CONFIG_AUTO_RPT): the nearest suitable repeater of the built-in table (FM, DMR on a DMR channel;
 *     RX and TX in the radio's bands); a new one is offered only once it is clearly nearer than the one offered
 *     before (ARPT_GAIN_PCT closer and ARPT_GAIN_M), checked every ARPT_EVERY_MS at most, never twice the same. */
#ifndef PLUS_MAPNAV_H
#define PLUS_MAPNAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui/map.h"
#include "aprs/aprs.h"
#if CONFIG_AUTO_RPT
#include "ui/rpt.h"
#endif

/* cos(latitude) Q15 (Bhaskara I's approximation, within 0.2 %) */
uint16_t mapnav_cos_q15(int32_t lat_e7);
/* The squared distance in square metres between two positions (equirectangular, at the first one's latitude: to
 * order and to compare with a threshold within a few hundred km) */
uint64_t mapnav_dist2_m(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7);

/* --- APRS stations heard (upstream's list) --- */

#define MAPNAV_ALT_NONE	INT16_MIN	/* aprs_station_t.alt_m: no altitude heard */
typedef bool (*mapnav_station_fn)(uint8_t i, aprs_station_t *out);
/* A callsign's hash, 1..65535 (0 is "none") */
uint16_t mapnav_call_hash(const char *call);
/* The station with a position after cur (a hash; 0: none, then the nearest) in the order nearest (lat, lon) first,
 * from the last one round to the nearest, of get(0 .. count - 1); 0 when no station has a position. A station that
 * went (cur not found) starts over at the nearest */
uint16_t mapnav_station_next(mapnav_station_fn get, uint8_t count, int32_t lat_e7, int32_t lon_e7, uint16_t cur);
/* The station with that hash and a position: its index, out filled; -1 when it is not (or no longer) in the list */
int mapnav_station_find(mapnav_station_fn get, uint8_t count, uint16_t hash, aprs_station_t *out);
/* The comment without its "/A=nnnnnn" altitude, the spaces around and doubled ones (out at least 1 byte, cut to
 * fit) */
void mapnav_comment(const char *text, char *out, size_t len);

/* --- Track --- */

#ifndef TRACK_POINTS
#define TRACK_POINTS		48u
#endif
#define TRACK_EVERY_MS		30000u
#define TRACK_MIN_M		50u

typedef struct {
	int16_t x, y;		/* map units (0.001 degree) east / north of TURKEY_MAP_LON0_E7 / TURKEY_MAP_LAT0_E7 */
} track_pt_t;

typedef struct {
	track_pt_t pt[TRACK_POINTS];	/* a ring: the oldest at (head - n) */
	int32_t start_lat, start_lon;	/* the first point since the clear, exactly */
	int32_t last_lat, last_lon;	/* where the newest point was taken, exactly (the TRACK_MIN_M rule) */
	uint32_t last_ms;
	uint8_t head, n;		/* the next slot, the points kept */
	bool started;
} track_t;

void track_clear(track_t *t);
/* A GPS fix: kept as the next point when it is the first one, or TRACK_EVERY_MS after the last one and TRACK_MIN_M
 * away from it (on another 0.001 degree cell); false when not kept. Outside the 16-bit range (about +-32 degrees
 * around the origin, 25 E 35.5 N) nothing is kept */
bool track_add(track_t *t, int32_t lat_e7, int32_t lon_e7, uint32_t now_ms);
static inline uint8_t track_count(const track_t *t) { return t->n; }
/* Point i, 0 = the oldest; false past the newest */
bool track_get(const track_t *t, uint8_t i, int32_t *lat_e7, int32_t *lon_e7);
/* The first point since the clear (it stays when the ring drops it); false with none */
bool track_start(const track_t *t, int32_t *lat_e7, int32_t *lon_e7);
/* Every segment between consecutive points through fn, clipped to the view (province argument 0xFF); returns how
 * many. The caller draws them dotted */
uint16_t track_draw(const map_view_t *v, const track_t *t, map_line_fn fn, void *ctx);
/* The ring slot of point i (0 = the oldest), for what is kept beside the ring (coverage) */
static inline uint8_t track_slot(const track_t *t, uint8_t i)
{
	return (uint8_t)((t->head + TRACK_POINTS - t->n + i) % TRACK_POINTS);
}

#if CONFIG_MAP_COVERAGE
/* --- Coverage (DDM32 Plus, CONFIG_MAP_COVERAGE): a byte beside each track point (the same ring slot: the track's
 * head and n), the signal strength of the channel or VFO in use when the point was taken; Tools > Map draws the
 * points coloured by it. The S-meter scale of VHF / UHF: S9 = -93 dBm, 6 dB a unit.
 * The byte: dBm + 141, 1 .. 140 (-140 .. -1 dBm, clamped); 0 = not known (the receiver gave no reading) */
#define COV_GOOD_DBM	(-105)	/* S7 and up: good (green) */
#define COV_FAIR_DBM	(-117)	/* S5 and S6: fair (yellow); weaker: poor (red) */
enum { COV_NONE, COV_POOR, COV_FAIR, COV_GOOD };
uint8_t cov_byte(int16_t dbm);
int16_t cov_dbm(uint8_t b);	/* 0 for COV_NONE's byte */
uint8_t cov_class(uint8_t b);	/* COV_* */
#endif

/* --- Auto repeater --- */

#if CONFIG_AUTO_RPT
#define ARPT_EVERY_MS		30000u	/* a check at most this often */
#define ARPT_GAIN_PCT		20u	/* another repeater must be this much nearer than the one offered ... */
#define ARPT_GAIN_M		2000u	/* ... and more than this */

typedef bool (*arpt_band_fn)(uint32_t hz, bool need_tx);	/* the radio can tune it (board_in_band) */

typedef struct {
	bool on;
	uint16_t cur;		/* the repeater offered last (or found tuned already), RPT_NONE */
	uint32_t next_ms;	/* the next check */
} arpt_t;

/* On (from the next check on, nothing offered yet) or off */
void arpt_set(arpt_t *s, bool on, uint32_t now_ms);
/* FM (DMR when dmr), RX and TX in the bands, a position */
bool arpt_suitable(const rpt_entry_t *e, bool dmr, arpt_band_fn in_band);
/* The nearest suitable repeater from (lat, lon), RPT_NONE without one */
uint16_t arpt_best(int32_t lat_e7, int32_t lon_e7, bool dmr, arpt_band_fn in_band);
/* One check, when due: the repeater to offer now (RPT_NONE: nothing new). The first suitable one after arpt_set(on)
 * is offered at once; then another only when it is ARPT_GAIN_PCT and ARPT_GAIN_M nearer than the one offered before
 * (or that one is no longer suitable: the mode changed). One the radio is tuned to already (tuned_hz its RX) becomes
 * the current one without an offer. Never the same one twice in a row */
uint16_t arpt_check(arpt_t *s, bool fix, int32_t lat_e7, int32_t lon_e7, bool dmr, arpt_band_fn in_band,
		    uint32_t tuned_hz, uint32_t now_ms);
#endif

/* --- The UI task's side (ui_mapnav.c) --- */

/* Every UI tick (ui_app.c): at most once a second the GPS fix into the track and the auto repeater's check; true:
 * draw again (the map is on top and got a track point) */
bool ui_mapnav_tick(uint32_t now_ms);
#if CONFIG_MAP_TRACK
extern track_t ui_mapnav_track;		/* drawn and cleared by Tools > Map */
#endif
#if CONFIG_MAP_COVERAGE
extern uint8_t ui_mapnav_cov[TRACK_POINTS];	/* cov_byte beside each point, by ring slot (track_slot) */
#endif
#if CONFIG_AUTO_RPT
bool ui_mapnav_auto_on(void);		/* Tools > Auto repeater's value */
void ui_mapnav_auto_toggle(void);	/* ... and OK on it */
void ui_mapnav_banner_open(void);	/* # on the offer's banner: tune (ui_rpt_tune) */
/* ui_app.c (prepare_mapnav): a banner of that kind over any screen, 8 s, # opens it (banner_open) */
void ui_banner_offer(uint8_t kind, const char *title, const char *text);
#endif
#if CONFIG_APRS_MSG
void ui_aprsmsg_compose_to(const char *to);	/* ui_aprsmsg.c: the composer to a station, on top of the map */
#endif

#endif
