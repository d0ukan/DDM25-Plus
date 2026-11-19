/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus SGP4 (docs/plus.md, Tools > Satellites): the NORAD simplified general perturbations model for near-earth
 * orbits (period under 225 minutes: every amateur FM satellite), written from the published description:
 *   F. R. Hoots, R. L. Roehrich, "Spacetrack Report #3: Models for Propagation of NORAD Element Sets", 1980;
 *   D. A. Vallado, P. Crawford, R. Hujsak, T. S. Kelso, "Revisiting Spacetrack Report #3", AIAA 2006-6753.
 * WGS-72 constants as in that paper ("wgs72"), the near-earth branch only (no SDP4 deep-space terms): deep-space
 * element sets are refused. Positions and velocities in the TEME frame, km and km/s. Double precision: the radio
 * has soft float only, one propagation is roughly a millisecond there (docs/plus.md). Pure math. */
#ifndef PLUS_SGP4_H
#define PLUS_SGP4_H

#include <stdint.h>

/* The mean elements of a two-line element set */
typedef struct {
	double epoch;		/* seconds since 2000-01-01 00:00 UTC */
	double no;		/* mean motion, radians a minute (the TLE's Kozai mean motion) */
	double ecc;		/* eccentricity */
	double inc, node, argp, mo;	/* inclination, right ascension of the node, argument of perigee, mean anomaly
					 * (radians) */
	double bstar;		/* drag term, 1 / earth radii */
} sgp4_elements_t;

/* The propagator's state after sgp4_init (about 300 bytes: kept on the caller's stack) */
typedef struct {
	double no, ecc, inc, node, argp, mo, bstar;
	double ao, sinio, cosio;	/* (XKE / no)^(2/3), sin and cos of the inclination: once, not every run */
	double aycof, con41, cc1, cc4, cc5, d2, d3, d4, delmo, eta, argpdot, omgcof, sinmao, t2cof, t3cof, t4cof,
	       t5cof, x1mth2, x7thm1, mdot, nodedot, xlcof, xmcof, nodecf;
	uint8_t isimp;
} sgp4_t;

enum {
	SGP4_OK,
	SGP4_BAD_ECC,		/* mean eccentricity out of range */
	SGP4_BAD_NO,		/* mean motion not positive */
	SGP4_BAD_PL,		/* semi-latus rectum negative */
	SGP4_DECAYED,		/* below the earth's surface */
	SGP4_DEEP,		/* deep-space element set (period 225 minutes or more): not supported */
};

#define SGP4_EARTH_KM	6378.135	/* WGS-72 equatorial radius */

int sgp4_init(sgp4_t *s, const sgp4_elements_t *e);
/* Position (km) and velocity (km/s) in TEME, tsince minutes after the epoch */
int sgp4_run(const sgp4_t *s, double tsince, double r[3], double v[3]);

#endif
