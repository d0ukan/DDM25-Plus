/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus satellites (CONFIG_SATELLITES, docs/plus.md): Tools > Satellites lists the next pass of every amateur
 * FM satellite in the table (tle.txt and sat_freq.txt beside this file, compiled in by tools/sat_tle.py) over the
 * GPS position, with the live look angles, range and Doppler of the one in focus, and OK tunes the VFO to it: RX the
 * downlink and TX the uplink, both corrected for Doppler as the pass goes. The PTT stays the user's. The pass alarm
 * sounds before a pass on any screen (sat_alarm_set, ui_sat_alarm).
 *
 * The math (sat.c: time, observer, look angles, Doppler, the pass search) is pure; the service (sat_svc.c) runs in
 * the radio task, at most a few SGP4 propagations a radio poll and only while the screen looks, a pass is tracked
 * or the alarm finds its pass (then a comparison a second until it is due), and keeps a small view the UI copies. */
#ifndef PLUS_SAT_H
#define PLUS_SAT_H

#include <stdbool.h>
#include <stdint.h>
#include "radio/sgp4.h"

#define SAT_MAX		8	/* satellites in the table at most */
#define SAT_NONE	0xFFu
#define SAT_NAME_LEN	11
#define SAT_LIGHT_KMS	299792.458
/* The ISS's APRS digipeater (ARISS): 145.825 MHz FM simplex, 1200 Bd AFSK, path ARISS. On the Satellites screen,
 * # on the ISS while it is above the horizon sends one position beacon there (aprs_iss_send, Doppler corrected) */
#define SAT_ISS_CATNUM	25544u
#define SAT_ISS_APRS_HZ	145825000u

/* One satellite: its element set and its FM transponder (sat_data.c, generated) */
typedef struct {
	char name[SAT_NAME_LEN + 1];
	uint32_t catnum;	/* NORAD catalog number */
	uint32_t down_hz, up_hz;	/* FM downlink and uplink (as the satellite hears and sends) */
	uint16_t ctcss10;	/* uplink CTCSS, Hz x 10 (670 = 67.0 Hz), 0 none */
	uint16_t arm10;		/* a second tone that switches the transponder on (SO-50: 74.4 Hz), 0 none */
	sgp4_elements_t el;
} sat_info_t;

extern const sat_info_t sat_table[];
extern const uint8_t sat_count;
extern const char sat_tle_note[];	/* the TLE file's first comment line (where the elements came from) */

/* --- Time: seconds since 2000-01-01 00:00 UTC --- */
/* From the GPS's ddmmyy and hhmmss (UTC); -1 for an impossible date */
int32_t sat_s2000(uint32_t ddmmyy, uint32_t hhmmss);
/* Greenwich mean sidereal time, radians (IAU 1982, as SGP4's TEME frame wants it) */
double sat_gmst(double s2000);

/* --- The observer: geodetic position on the WGS-84 ellipsoid, earth-fixed --- */
typedef struct {
	double x, y, z;		/* km, earth-fixed */
	double slat, clat, slon, clon;
} sat_obs_t;
void sat_observer(int32_t lat_e7, int32_t lon_e7, int32_t alt_m, sat_obs_t *o);

/* --- Where the satellite is seen from --- */
typedef struct {
	float az, el;		/* degrees: azimuth from north, clockwise; elevation */
	float range_km;
	float rate_kms;		/* range rate, km/s, positive moving away */
	float gap;		/* radians the sub-satellite point is beyond the observer's horizon circle (<= 0 inside) */
	float omega;		/* the fastest the gap can close, radians a second */
} sat_look_t;
/* p initialized for info's elements; t seconds since 2000. SGP4_* */
int sat_eval(const sgp4_t *p, const sat_info_t *info, double t, const sat_obs_t *o, sat_look_t *out);

/* --- Doppler --- */
/* The frequency the radio receives on (from) or must send on (to) for a satellite moving at rate_kms */
int32_t sat_doppler_rx(uint32_t hz, float rate_kms);	/* the shift to add to the downlink */
int32_t sat_doppler_tx(uint32_t hz, float rate_kms);	/* the shift to add to the uplink */
/* The step the corrections move in: 1 kHz on VHF, 2.5 kHz above 300 MHz (FM is that tolerant; fewer retunes). A
 * policy, not the hardware's: the FD6818 synthesizer is set in 10 Hz units (registers 0x38 / 0x39) */
uint32_t sat_step_hz(uint32_t hz);
/* hz plus a shift rounded to its step */
uint32_t sat_tune_hz(uint32_t hz, int32_t shift);
/* The tracking's next frequency: tuned_hz (what the VFO is on now, 0 none) while hz + shift is within
 * SAT_HOLD_8THS / 8 of a step of it, else hz + shift rounded to the step (sat_tune_hz). So the VFO moves only once the
 * correction is more than half a step (plus a margin) off, and never back and forth over a step's edge when the clock
 * is corrected by a second */
#define SAT_HOLD_8THS	5
uint32_t sat_follow_hz(uint32_t hz, int32_t shift, uint32_t tuned_hz);

/* --- Passes --- */
enum { SAT_PASS_UNKNOWN, SAT_PASS_OK, SAT_PASS_NONE, SAT_PASS_BAD };
typedef struct {
	int32_t aos, los;	/* rise and set, seconds since 2000 (aos in the past: under way) */
	int8_t max_el;		/* the highest elevation, degrees */
	uint8_t state;		/* SAT_PASS_*: NONE = no pass in SAT_SEARCH_S, BAD = the elements don't propagate */
} sat_pass_t;

#define SAT_SEARCH_S	(36L * 3600L)	/* how far ahead a pass is looked for */

/* The pass search, one propagation a step (so the radio task can do it a little at a time) */
typedef struct {
	uint8_t phase;
	int32_t start, t, prev, lo, hi;
	float best_el, e1;
	int32_t best_t;
	sat_pass_t out;
} sat_search_t;
void sat_search_start(sat_search_t *s, int32_t now);
/* One propagation of the search; true once s->out is final */
bool sat_search_step(sat_search_t *s, const sgp4_t *p, const sat_info_t *info, const sat_obs_t *o);

/* --- The service (sat_svc.c, radio task) and what the UI sees --- */
enum { SAT_CLOCK_NONE, SAT_CLOCK_NO_POS, SAT_CLOCK_OK };
enum { SAT_TRK_OFF, SAT_TRK_ON, SAT_TRK_LOS, SAT_TRK_LEFT };	/* SAT_TRK_LEFT: the VFO was changed by hand */

/* The pass alarm (RAM only: off after a power cycle). Armed for one satellite's next pass or for the next pass of any
 * satellite, either with a lowest top elevation; it sounds lead_min minutes before the rise (SAT_ALARM_SOON) and at
 * the rise (SAT_ALARM_AOS), then it is off. The radio task finds the pass once (SGP4, a propagation a poll, or the
 * screen's own search result when it has one), then only compares the time, once a second, until it is due. */
#define SAT_ALARM_ANY	0xFEu	/* sat_alarm_set target: any satellite (SAT_NONE: off) */
enum { SAT_AL_OFF, SAT_AL_FINDING, SAT_AL_ARMED, SAT_AL_NONE };	/* SAT_AL_NONE: no such pass in SAT_SEARCH_S */
enum { SAT_ALARM_SOON = 1, SAT_ALARM_AOS };
typedef struct {
	uint8_t target;		/* a satellite, SAT_ALARM_ANY, or SAT_NONE (off) */
	uint8_t lead_min;	/* minutes before the rise */
	uint8_t min_el;		/* the lowest top elevation, degrees */
	uint8_t state;		/* SAT_AL_* */
	uint8_t sat;		/* SAT_AL_ARMED: the pass's satellite ... */
	int8_t max_el;		/* ... its top elevation ... */
	bool soon;		/* ... the warning given */
	int32_t aos;		/* ... and its rise, seconds since 2000 */
} sat_alarm_t;
typedef struct {
	uint8_t kind;		/* SAT_ALARM_SOON or SAT_ALARM_AOS */
	uint8_t sat;
	int8_t max_el;
	int32_t in_s;		/* seconds to the rise (SOON) */
} sat_alarm_event_t;

typedef struct {
	uint8_t clock;		/* SAT_CLOCK_* */
	uint8_t live;		/* the satellite of the live fields (the focus, or the one tracked), SAT_NONE */
	uint8_t track;		/* the satellite tracked, SAT_NONE */
	uint8_t track_state;	/* SAT_TRK_* (why the last tracking ended) */
	bool arm;		/* the arming tone is on instead of the satellite's CTCSS */
	bool live_ok;
	int16_t az10, el10;	/* degrees x 10 */
	uint16_t range_km;
	int16_t rate_ms;	/* range rate, m/s, positive moving away */
	uint16_t cost_us;	/* the slowest propagation measured (CPU cost on this radio) */
	int32_t now;		/* seconds since 2000 UTC */
	uint32_t rx_hz, tx_hz;	/* what the tracking tuned */
	sat_alarm_t alarm;
	sat_pass_t pass[SAT_MAX];
} sat_view_t;

/* UI task */
void sat_view(sat_view_t *out);
void sat_seen(uint8_t focus);		/* the screen looks (from its draw), the satellite in focus */
void sat_track(uint8_t idx);		/* follow this satellite's pass in the VFO; SAT_NONE stops */
void sat_arm(bool on);			/* the arming tone instead of the CTCSS on the uplink */
/* Arm the pass alarm (target SAT_NONE: off), replacing one armed before */
void sat_alarm_set(uint8_t target, uint8_t lead_min, uint8_t min_el);
/* Every UI tick, whatever screen is open: true once for each alarm due, *out what it is about */
bool sat_alarm_take(sat_alarm_event_t *out);
/* ui_sat.c, from ui_app_tick: an alarm due sounds its tone and returns its text ("ISS in 5 min, max 47°", a static
 * buffer) for the banner; NULL when none */
const char *ui_sat_alarm(void);

/* Radio task, every radio poll: the VFO as it is now (to see a change by hand); light: the native receiver owns the
 * radio (a P25 channel), one propagation a poll at most */
void sat_service(bool vfo, uint32_t rx_hz, uint32_t tx_hz, bool transmitting, bool light, uint32_t now_ms);

#endif
