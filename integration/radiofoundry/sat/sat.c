/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus satellites, the math (sat.h): UTC to sidereal time, the observer on the WGS-84 ellipsoid, SGP4's TEME
 * position turned earth-fixed (the equation of the equinoxes and polar motion left out: under a few arc seconds),
 * look angles and range rate from the topocentric south-east-zenith frame (Vallado, "Fundamentals of Astrodynamics
 * and Applications", the RAZEL algorithm), Doppler, and the pass search. */
#include "config.h"
#if CONFIG_SATELLITES
#include <math.h>
#include <string.h>
#include "radio/sat.h"

#define PI		3.14159265358979323846
#define DEG		(PI / 180.0)
#define OMEGA_E		7.292115e-5		/* earth's rotation, rad/s */
#define WGS84_A		6378.137
#define WGS84_F		(1.0 / 298.257223563)

static int32_t days2000(int y, int m, int d)
{
	static const uint16_t before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	bool leap = y % 4 == 0;		/* 2000 .. 2099 */
	return (int32_t)(y - 2000) * 365 + (y - 1997) / 4 + before[m - 1] + (d - 1) + (leap && m > 2 ? 1 : 0);
}

int32_t sat_s2000(uint32_t ddmmyy, uint32_t hhmmss)
{
	int d = (int)(ddmmyy / 10000u), m = (int)(ddmmyy / 100u % 100u), y = 2000 + (int)(ddmmyy % 100u);
	uint32_t h = hhmmss / 10000u, mi = hhmmss / 100u % 100u, s = hhmmss % 100u;
	if (d < 1 || d > 31 || m < 1 || m > 12 || y > 2067 || h > 23 || mi > 59 || s > 60)
		return -1;	/* (32-bit seconds end in January 2068) */
	return days2000(y, m, d) * 86400 + (int32_t)(h * 3600u + mi * 60u + s);
}

double sat_gmst(double s2000)
{
	double tut1 = (s2000 / 86400.0 - 0.5) / 36525.0;	/* Julian centuries from J2000.0 (UT1 ~ UTC) */
	double sec = ((-6.2e-6 * tut1 + 0.093104) * tut1 + (876600.0 * 3600.0 + 8640184.812866)) * tut1 + 67310.54841;
	double g = fmod(sec * DEG / 240.0, 2.0 * PI);
	return g < 0.0 ? g + 2.0 * PI : g;
}

void sat_observer(int32_t lat_e7, int32_t lon_e7, int32_t alt_m, sat_obs_t *o)
{
	double lat = (double)lat_e7 * 1e-7 * DEG, lon = (double)lon_e7 * 1e-7 * DEG, h = (double)alt_m / 1000.0;
	const double e2 = WGS84_F * (2.0 - WGS84_F);
	o->slat = sin(lat);
	o->clat = cos(lat);
	o->slon = sin(lon);
	o->clon = cos(lon);
	double n = WGS84_A / sqrt(1.0 - e2 * o->slat * o->slat);
	o->x = (n + h) * o->clat * o->clon;
	o->y = (n + h) * o->clat * o->slon;
	o->z = (n * (1.0 - e2) + h) * o->slat;
}

int sat_eval(const sgp4_t *p, const sat_info_t *info, double t, const sat_obs_t *o, sat_look_t *out)
{
	double r[3], v[3];
	int err = sgp4_run(p, (t - info->el.epoch) / 60.0, r, v);
	if (err != SGP4_OK)
		return err;
	double th = sat_gmst(t), st = sin(th), ct = cos(th);
	/* TEME to earth-fixed: a turn about z by the sidereal angle; the velocity loses the frame's rotation */
	double xe = ct * r[0] + st * r[1], ye = -st * r[0] + ct * r[1], ze = r[2];
	double vxe = ct * v[0] + st * v[1] + OMEGA_E * ye, vye = -st * v[0] + ct * v[1] - OMEGA_E * xe, vze = v[2];
	double rx = xe - o->x, ry = ye - o->y, rz = ze - o->z;
	double range = sqrt(rx * rx + ry * ry + rz * rz);
	double south = o->slat * o->clon * rx + o->slat * o->slon * ry - o->clat * rz;
	double east = -o->slon * rx + o->clon * ry;
	double zen = o->clat * o->clon * rx + o->clat * o->slon * ry + o->slat * rz;
	double az = atan2(east, -south) / DEG;
	out->az = (float)(az < 0.0 ? az + 360.0 : az);
	out->el = (float)(asin(zen / range) / DEG);
	out->range_km = (float)range;
	out->rate_kms = (float)((rx * vxe + ry * vye + rz * vze) / range);
	/* How far outside the circle it can be seen from: the central angle to the sub-satellite point against the
	 * horizon's (a sphere through the observer; 0.02 rad covers the ellipsoid and an eccentric orbit's height) */
	double rs = sqrt(xe * xe + ye * ye + ze * ze), ro = sqrt(o->x * o->x + o->y * o->y + o->z * o->z);
	double c = (xe * o->x + ye * o->y + ze * o->z) / (rs * ro);
	c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
	out->gap = (float)(acos(c) - acos(ro / rs) - 0.02);
	out->omega = (float)(p->no / 60.0 * (1.05 + 2.0 * p->ecc) + OMEGA_E);
	return SGP4_OK;
}

int32_t sat_doppler_rx(uint32_t hz, float rate_kms)
{
	return (int32_t)lrint(-(double)hz * (double)rate_kms / SAT_LIGHT_KMS);
}

int32_t sat_doppler_tx(uint32_t hz, float rate_kms)
{
	/* What the satellite hears is hz (1 - rate / c): send hz / (1 - rate / c) */
	return (int32_t)lrint((double)hz * (double)rate_kms / (SAT_LIGHT_KMS - (double)rate_kms));
}

uint32_t sat_step_hz(uint32_t hz)
{
	return hz >= 300000000u ? 2500u : 1000u;
}

uint32_t sat_tune_hz(uint32_t hz, int32_t shift)
{
	int32_t step = (int32_t)sat_step_hz(hz);
	int32_t q = shift >= 0 ? (shift + step / 2) / step : -((-shift + step / 2) / step);
	return (uint32_t)((int32_t)hz + q * step);
}

uint32_t sat_follow_hz(uint32_t hz, int32_t shift, uint32_t tuned_hz)
{
	int32_t off = (int32_t)(hz - tuned_hz) + shift;		/* where it should be against where it is */
	int32_t hold = (int32_t)sat_step_hz(hz) * SAT_HOLD_8THS / 8;
	if (tuned_hz != 0 && off <= hold && off >= -hold)
		return tuned_hz;
	return sat_tune_hz(hz, shift);
}

/* The pass search: from the start, steps that cannot jump over the visible circle (the gap closes at most at omega)
 * until it rises, bisection to the rise, 30 s steps to the set, bisection to it, and a ternary search for the
 * highest elevation around the best sample. Passes under 20 s above the horizon can be missed. */
enum { S_START, S_BACK, S_AOS, S_AOS_BIS, S_LOS, S_LOS_BIS, S_MAX1, S_MAX2, S_DONE };
#define MIN_STEP_S	20
#define MAX_STEP_S	1800
#define BACK_STEP_S	60
#define LOS_STEP_S	30
#define BIS_RES_S	2
#define MAX_RES_S	3
#define LONGEST_S	7200

void sat_search_start(sat_search_t *s, int32_t now)
{
	memset(s, 0, sizeof *s);
	s->phase = S_START;
	s->start = s->t = s->prev = now;
	s->best_el = -90.0f;
	s->out.state = SAT_PASS_UNKNOWN;
}

static int32_t safe_step(const sat_look_t *l)
{
	if (l->gap <= 0.0f)
		return MIN_STEP_S;
	float st = 0.9f * l->gap / l->omega;
	return st < (float)MIN_STEP_S ? MIN_STEP_S : st > (float)MAX_STEP_S ? MAX_STEP_S : (int32_t)st;
}

static void max_start(sat_search_t *s)
{
	s->lo = s->best_t - LOS_STEP_S < s->out.aos ? s->out.aos : s->best_t - LOS_STEP_S;
	s->hi = s->best_t + LOS_STEP_S > s->out.los ? s->out.los : s->best_t + LOS_STEP_S;
	s->phase = s->hi - s->lo <= MAX_RES_S ? S_DONE : S_MAX1;
}

static void finish(sat_search_t *s)
{
	s->out.max_el = (int8_t)(s->best_el + 0.5f);
	s->out.state = SAT_PASS_OK;
	s->phase = S_DONE;
}

bool sat_search_step(sat_search_t *s, const sgp4_t *p, const sat_info_t *info, const sat_obs_t *o)
{
	int32_t t;
	switch (s->phase) {
	case S_START: case S_BACK: case S_AOS: case S_LOS:
		t = s->t;
		break;
	case S_AOS_BIS: case S_LOS_BIS:
		t = s->lo + (s->hi - s->lo) / 2;
		break;
	case S_MAX1:
		t = s->lo + (s->hi - s->lo) / 3;
		break;
	case S_MAX2:
		t = s->hi - (s->hi - s->lo) / 3;
		break;
	default:
		return true;
	}
	sat_look_t l;
	if (sat_eval(p, info, (double)t, o, &l) != SGP4_OK) {
		s->out.state = SAT_PASS_BAD;
		s->phase = S_DONE;
		return true;
	}
	bool up = l.el >= 0.0f;
	if (up && l.el > s->best_el) {
		s->best_el = l.el;
		s->best_t = t;
	}
	switch (s->phase) {
	case S_START:
		s->prev = t;
		if (up) {
			s->phase = S_BACK;	/* under way: back to its rise */
			s->t = t - BACK_STEP_S;
		} else {
			s->phase = S_AOS;
			s->t = t + safe_step(&l);
		}
		break;
	case S_BACK:
		if (!up) {
			s->lo = t;
			s->hi = s->prev;
			s->phase = S_AOS_BIS;
		} else if (s->start - t > LONGEST_S) {
			s->lo = s->hi = t;	/* never set: give the start as the rise */
			s->phase = S_AOS_BIS;
		} else {
			s->prev = t;
			s->t = t - BACK_STEP_S;
		}
		break;
	case S_AOS:
		if (up) {
			s->lo = s->prev;
			s->hi = t;
			s->phase = S_AOS_BIS;
		} else if (t - s->start > SAT_SEARCH_S) {
			s->out.state = SAT_PASS_NONE;
			s->phase = S_DONE;
		} else {
			s->prev = t;
			s->t = t + safe_step(&l);
		}
		break;
	case S_AOS_BIS:
		if (up)
			s->hi = t;
		else
			s->lo = t;
		if (s->hi - s->lo <= BIS_RES_S) {
			s->out.aos = s->hi;
			s->prev = s->hi > s->start ? s->hi : s->start;	/* known up */
			s->t = s->prev + LOS_STEP_S;
			s->phase = S_LOS;
		}
		break;
	case S_LOS:
		if (!up) {
			s->lo = s->prev;
			s->hi = t;
			s->phase = S_LOS_BIS;
		} else if (t - s->out.aos > LONGEST_S) {
			s->out.los = t;
			max_start(s);
			if (s->phase == S_DONE)
				finish(s);
		} else {
			s->prev = t;
			s->t = t + LOS_STEP_S;
		}
		break;
	case S_LOS_BIS:
		if (up)
			s->lo = t;
		else
			s->hi = t;
		if (s->hi - s->lo <= BIS_RES_S) {
			s->out.los = s->lo;
			max_start(s);
			if (s->phase == S_DONE)
				finish(s);
		}
		break;
	case S_MAX1:
		s->e1 = l.el;
		s->phase = S_MAX2;
		break;
	case S_MAX2:
		if (s->e1 < l.el)
			s->lo = s->lo + (s->hi - s->lo) / 3;
		else
			s->hi = t;
		if (s->hi - s->lo <= MAX_RES_S)
			finish(s);
		else
			s->phase = S_MAX1;
		break;
	}
	return s->phase == S_DONE;
}
#endif
