/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus SGP4, near-earth branch (sgp4.h). The equations follow the order of Vallado et al. 2006 ("Revisiting
 * Spacetrack Report #3", sections on initialization and the near-earth propagation), WGS-72 constants, so the
 * results reproduce that paper's verification output for near-earth element sets (tests/plus/test_sat.c). */
#include "config.h"
#if CONFIG_SATELLITES
#include <math.h>
#include "radio/sgp4.h"

#define TWOPI		6.283185307179586476925287
#define X2O3		(2.0 / 3.0)
#define MU		398600.8		/* km^3/s^2, WGS-72 */
#define RE		SGP4_EARTH_KM
#define XKE		0.0743669161331734132	/* 60 / sqrt(RE^3 / MU): sqrt of earth radii^3 / min^2 */
#define J2		0.001082616
#define J3		(-0.00000253881)
#define J4		(-0.00000165597)
#define J3OJ2		(J3 / J2)
#define VKMPERSEC	(RE * XKE / 60.0)

int sgp4_init(sgp4_t *s, const sgp4_elements_t *e)
{
	const double ss = 78.0 / RE + 1.0;
	const double qz = (120.0 - 78.0) / RE;
	const double qzms2t = qz * qz * qz * qz;
	const double temp4 = 1.5e-12;

	s->ecc = e->ecc;
	s->inc = e->inc;
	s->node = e->node;
	s->argp = e->argp;
	s->mo = e->mo;
	s->bstar = e->bstar;
	if (e->no <= 0.0)
		return SGP4_BAD_NO;
	if (e->ecc < 0.0 || e->ecc >= 1.0)
		return SGP4_BAD_ECC;

	/* Initialization: the Brouwer (un-Kozai'd) mean motion and the semi-major axis */
	double eccsq = e->ecc * e->ecc;
	double omeosq = 1.0 - eccsq;
	double rteosq = sqrt(omeosq);
	double cosio = cos(e->inc);
	double cosio2 = cosio * cosio;
	double ak = cbrt(XKE / e->no * (XKE / e->no));	/* (XKE / no)^(2/3); cbrt is cheaper than pow */
	double d1 = 0.75 * J2 * (3.0 * cosio2 - 1.0) / (rteosq * omeosq);
	double del = d1 / (ak * ak);
	double adel = ak * (1.0 - del * del - del * (1.0 / 3.0 + 134.0 * del * del / 81.0));
	del = d1 / (adel * adel);
	s->no = e->no / (1.0 + del);
	if (TWOPI / s->no >= 225.0)
		return SGP4_DEEP;
	double ao = cbrt(XKE / s->no * (XKE / s->no));
	double sinio = sin(e->inc);
	s->ao = ao;
	s->sinio = sinio;
	s->cosio = cosio;
	double po = ao * omeosq;
	double con42 = 1.0 - 5.0 * cosio2;
	s->con41 = -con42 - cosio2 - cosio2;
	double posq = po * po;
	double rp = ao * (1.0 - e->ecc);

	s->isimp = rp < 220.0 / RE + 1.0;
	double sfour = ss, qzms24 = qzms2t;
	double perige = (rp - 1.0) * RE;
	if (perige < 156.0) {	/* low perigee: the atmosphere's density parameter moves down */
		sfour = perige - 78.0;
		if (perige < 98.0)
			sfour = 20.0;
		double q = (120.0 - sfour) / RE;
		qzms24 = q * q * q * q;
		sfour = sfour / RE + 1.0;
	}
	double pinvsq = 1.0 / posq;
	double tsi = 1.0 / (ao - sfour);
	s->eta = ao * e->ecc * tsi;
	double etasq = s->eta * s->eta;
	double eeta = e->ecc * s->eta;
	double psisq = fabs(1.0 - etasq);
	double tsi2 = tsi * tsi;
	double coef = qzms24 * tsi2 * tsi2;
	double coef1 = coef / (psisq * psisq * psisq * sqrt(psisq));	/* psisq^3.5 */
	double cc2 = coef1 * s->no * (ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq)) +
		0.375 * J2 * tsi / psisq * s->con41 * (8.0 + 3.0 * etasq * (8.0 + etasq)));
	s->cc1 = e->bstar * cc2;
	double cc3 = 0.0;
	if (e->ecc > 1.0e-4)
		cc3 = -2.0 * coef * tsi * J3OJ2 * s->no * sinio / e->ecc;
	s->x1mth2 = 1.0 - cosio2;
	s->cc4 = 2.0 * s->no * coef1 * ao * omeosq * (s->eta * (2.0 + 0.5 * etasq) + e->ecc * (0.5 + 2.0 * etasq) -
		J2 * tsi / (ao * psisq) * (-3.0 * s->con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta)) +
		0.75 * s->x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) * cos(2.0 * e->argp)));
	s->cc5 = 2.0 * coef1 * ao * omeosq * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);
	double cosio4 = cosio2 * cosio2;
	double temp1 = 1.5 * J2 * pinvsq * s->no;
	double temp2 = 0.5 * temp1 * J2 * pinvsq;
	double temp3 = -0.46875 * J4 * pinvsq * pinvsq * s->no;
	s->mdot = s->no + 0.5 * temp1 * rteosq * s->con41 + 0.0625 * temp2 * rteosq * (13.0 - 78.0 * cosio2 + 137.0 * cosio4);
	s->argpdot = -0.5 * temp1 * con42 + 0.0625 * temp2 * (7.0 - 114.0 * cosio2 + 395.0 * cosio4) +
		temp3 * (3.0 - 36.0 * cosio2 + 49.0 * cosio4);
	double xhdot1 = -temp1 * cosio;
	s->nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * cosio2) + 2.0 * temp3 * (3.0 - 7.0 * cosio2)) * cosio;
	s->omgcof = e->bstar * cc3 * cos(e->argp);
	s->xmcof = 0.0;
	if (e->ecc > 1.0e-4)
		s->xmcof = -X2O3 * coef * e->bstar / eeta;
	s->nodecf = 3.5 * omeosq * xhdot1 * s->cc1;
	s->t2cof = 1.5 * s->cc1;
	if (fabs(cosio + 1.0) > 1.5e-12)
		s->xlcof = -0.25 * J3OJ2 * sinio * (3.0 + 5.0 * cosio) / (1.0 + cosio);
	else
		s->xlcof = -0.25 * J3OJ2 * sinio * (3.0 + 5.0 * cosio) / temp4;
	s->aycof = -0.5 * J3OJ2 * sinio;
	double delmotemp = 1.0 + s->eta * cos(e->mo);
	s->delmo = delmotemp * delmotemp * delmotemp;
	s->sinmao = sin(e->mo);
	s->x7thm1 = 7.0 * cosio2 - 1.0;
	s->d2 = s->d3 = s->d4 = s->t3cof = s->t4cof = s->t5cof = 0.0;
	if (!s->isimp) {
		double cc1sq = s->cc1 * s->cc1;
		s->d2 = 4.0 * ao * tsi * cc1sq;
		double temp = s->d2 * tsi * s->cc1 / 3.0;
		s->d3 = (17.0 * ao + sfour) * temp;
		s->d4 = 0.5 * temp * ao * tsi * (221.0 * ao + 31.0 * sfour) * s->cc1;
		s->t3cof = s->d2 + 2.0 * cc1sq;
		s->t4cof = 0.25 * (3.0 * s->d3 + s->cc1 * (12.0 * s->d2 + 10.0 * cc1sq));
		s->t5cof = 0.2 * (3.0 * s->d4 + 12.0 * s->cc1 * s->d3 + 6.0 * s->d2 * s->d2 + 15.0 * cc1sq * (2.0 * s->d2 + cc1sq));
	}
	return SGP4_OK;
}

int sgp4_run(const sgp4_t *s, double t, double r[3], double v[3])
{
	/* Secular gravity and atmospheric drag */
	double xmdf = s->mo + s->mdot * t;
	double argpdf = s->argp + s->argpdot * t;
	double nodedf = s->node + s->nodedot * t;
	double argpm = argpdf, mm = xmdf;
	double t2 = t * t;
	double nodem = nodedf + s->nodecf * t2;
	double tempa = 1.0 - s->cc1 * t;
	double tempe = s->bstar * s->cc4 * t;
	double templ = s->t2cof * t2;
	if (!s->isimp) {
		double delomg = s->omgcof * t;
		double delmtemp = 1.0 + s->eta * cos(xmdf);
		double delm = s->xmcof * (delmtemp * delmtemp * delmtemp - s->delmo);
		double temp = delomg + delm;
		mm = xmdf + temp;
		argpm = argpdf - temp;
		double t3 = t2 * t, t4 = t3 * t;
		tempa = tempa - s->d2 * t2 - s->d3 * t3 - s->d4 * t4;
		tempe = tempe + s->bstar * s->cc5 * (sin(mm) - s->sinmao);
		templ = templ + s->t3cof * t3 + t4 * (s->t4cof + t * s->t5cof);
	}
	double am = s->ao * tempa * tempa;
	double sqrt_am = sqrt(am);
	double nm = XKE / (am * sqrt_am);	/* XKE / am^1.5 */
	double em = s->ecc - tempe;
	if (em >= 1.0 || em < -0.001)
		return SGP4_BAD_ECC;
	if (em < 1.0e-6)
		em = 1.0e-6;
	mm = mm + s->no * templ;
	double xlm = mm + argpm + nodem;
	nodem = fmod(nodem, TWOPI);
	argpm = fmod(argpm, TWOPI);
	xlm = fmod(xlm, TWOPI);
	mm = fmod(xlm - argpm - nodem, TWOPI);
	double sinip = s->sinio, cosip = s->cosio;

	/* Long-period periodics */
	double axnl = em * cos(argpm);
	double temp = 1.0 / (am * (1.0 - em * em));
	double aynl = em * sin(argpm) + temp * s->aycof;
	double xl = mm + argpm + nodem + temp * s->xlcof * axnl;

	/* Kepler's equation */
	double u = fmod(xl - nodem, TWOPI);
	double eo1 = u, tem5 = 9999.9, sineo1 = 0.0, coseo1 = 1.0;
	for (int k = 1; fabs(tem5) >= 1.0e-12 && k <= 10; k++) {
		sineo1 = sin(eo1);
		coseo1 = cos(eo1);
		tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl;
		tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
		if (fabs(tem5) >= 0.95)
			tem5 = tem5 > 0.0 ? 0.95 : -0.95;
		eo1 = eo1 + tem5;
	}

	/* Short-period periodics */
	double ecose = axnl * coseo1 + aynl * sineo1;
	double esine = axnl * sineo1 - aynl * coseo1;
	double el2 = axnl * axnl + aynl * aynl;
	double pl = am * (1.0 - el2);
	if (pl < 0.0)
		return SGP4_BAD_PL;
	double rl = am * (1.0 - ecose);
	double rdotl = sqrt_am * esine / rl;
	double rvdotl = sqrt(pl) / rl;
	double betal = sqrt(1.0 - el2);
	temp = esine / (1.0 + betal);
	double sinu = am / rl * (sineo1 - aynl - axnl * temp);
	double cosu = am / rl * (coseo1 - axnl + aynl * temp);
	double su = atan2(sinu, cosu);
	double sin2u = (cosu + cosu) * sinu;
	double cos2u = 1.0 - 2.0 * sinu * sinu;
	temp = 1.0 / pl;
	double temp1 = 0.5 * J2 * temp;
	double temp2 = temp1 * temp;
	double mrt = rl * (1.0 - 1.5 * temp2 * betal * s->con41) + 0.5 * temp1 * s->x1mth2 * cos2u;
	su = su - 0.25 * temp2 * s->x7thm1 * sin2u;
	double xnode = nodem + 1.5 * temp2 * cosip * sin2u;
	double xinc = s->inc + 1.5 * temp2 * cosip * sinip * cos2u;
	double mvt = rdotl - nm * temp1 * s->x1mth2 * sin2u / XKE;
	double rvdot = rvdotl + nm * temp1 * (s->x1mth2 * cos2u + 1.5 * s->con41) / XKE;

	/* Orientation vectors */
	double sinsu = sin(su), cossu = cos(su), snod = sin(xnode), cnod = cos(xnode), sini = sin(xinc), cosi = cos(xinc);
	double xmx = -snod * cosi, xmy = cnod * cosi;
	double ux = xmx * sinsu + cnod * cossu, uy = xmy * sinsu + snod * cossu, uz = sini * sinsu;
	double vx = xmx * cossu - cnod * sinsu, vy = xmy * cossu - snod * sinsu, vz = sini * cossu;
	r[0] = mrt * ux * RE;
	r[1] = mrt * uy * RE;
	r[2] = mrt * uz * RE;
	v[0] = (mvt * ux + rvdot * vx) * VKMPERSEC;
	v[1] = (mvt * uy + rvdot * vy) * VKMPERSEC;
	v[2] = (mvt * uz + rvdot * vz) * VKMPERSEC;
	return mrt < 1.0 ? SGP4_DECAYED : SGP4_OK;
}
#endif
