/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus satellites (radio/sgp4.c, radio/sat.c, the generated radio/sat_data.c):
 *  - SGP4 against the verification output of Vallado et al. 2006 ("Revisiting Spacetrack Report #3", the near-earth
 *    cases 00005, 06251 and 28057 of its test set), WGS-72;
 *  - physical checks on ISS-like elements: period, height, energy, two-body agreement over a short span, the range
 *    rate against the range's own derivative;
 *  - sidereal time (Vallado's example 3-5 and the J2000 value), the date conversion against timegm();
 *  - the pass search against a one-second brute-force scan (no pass missed, rise / set / highest elevation);
 *  - Doppler sign and size, the retune step; the shipped table propagates. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "radio/sat.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define PI 3.14159265358979323846

static double field(const char *line, int from, int to)
{
	char buf[24];
	int n = to - from + 1;
	memcpy(buf, line + from - 1, (size_t)n);
	buf[n] = 0;
	return atof(buf);
}

static double implied(const char *line, int from)
{
	char m[8], e[3];
	memcpy(m, line + from, 5);	/* the 5 digits after the sign column */
	m[5] = 0;
	memcpy(e, line + from + 5, 2);
	e[2] = 0;
	double v = atof(m) * 1e-5 * pow(10.0, atof(e));
	return line[from - 1] == '-' ? -v : v;
}

/* Columns as in the TLE format; the epoch from 2000 in seconds (tests only need differences) */
static void parse(const char *l1, const char *l2, sgp4_elements_t *e)
{
	int yy = (int)field(l1, 19, 20);
	int year = yy < 57 ? 2000 + yy : 1900 + yy;
	double days = 0;
	for (int y = 2000; y < year; y++)
		days += (y % 4 == 0) ? 366 : 365;
	for (int y = year; y < 2000; y++)
		days -= (y % 4 == 0) ? 366 : 365;
	e->epoch = (days + field(l1, 21, 32) - 1.0) * 86400.0;
	e->bstar = implied(l1, 54);
	e->inc = field(l2, 9, 16) * PI / 180.0;
	e->node = field(l2, 18, 25) * PI / 180.0;
	char ecc[12] = "0.";
	memcpy(ecc + 2, l2 + 26, 7);
	ecc[9] = 0;
	e->ecc = atof(ecc);
	e->argp = field(l2, 35, 42) * PI / 180.0;
	e->mo = field(l2, 44, 51) * PI / 180.0;
	e->no = field(l2, 53, 63) * 2.0 * PI / 1440.0;
}

typedef struct {
	double t, r[3], v[3];
} vec_t;

static void check_vectors(const char *name, const char *l1, const char *l2, const vec_t *want, int n)
{
	sgp4_elements_t e;
	sgp4_t s;
	parse(l1, l2, &e);
	CHECK(sgp4_init(&s, &e) == SGP4_OK);
	double worst_r = 0, worst_v = 0;
	for (int i = 0; i < n; i++) {
		double r[3], v[3];
		CHECK(sgp4_run(&s, want[i].t, r, v) == SGP4_OK);
		for (int k = 0; k < 3; k++) {
			worst_r = fmax(worst_r, fabs(r[k] - want[i].r[k]));
			worst_v = fmax(worst_v, fabs(v[k] - want[i].v[k]));
		}
	}
	printf("  SGP4 %s: %d epochs, worst %.2e km, %.2e km/s off Vallado's output\n", name, n, worst_r, worst_v);
	CHECK(worst_r < 1e-4);		/* 10 cm */
	CHECK(worst_v < 1e-7);
}

static void sgp4_vectors(void)
{
	/* Vallado et al. 2006, SGP4-VER.TLE and its tcppver.out (WGS-72) */
	static const vec_t v00005[] = {
		{ 0.0, { 7022.46529266, -1400.08296755, 0.03995155 }, { 1.893841015, 6.405893759, 4.534807250 } },
		{ 360.0, { -7154.03120202, -3783.17682504, -3536.19412294 }, { 4.741887409, -4.151817765, -2.093935425 } },
		{ 720.0, { -7134.59340119, 6531.68641334, 3260.27186483 }, { -4.113793027, -2.911922039, -2.557327851 } },
		{ 1080.0, { 5568.53901181, 4492.06992591, 3863.87641983 }, { -4.209106476, 5.159719888, 2.744852980 } },
		{ 1440.0, { -938.55923943, -6268.18748831, -4294.02924751 }, { 7.536105209, -0.427127707, 0.989878080 } },
	};
	check_vectors("00005", "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
		      "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667", v00005, 5);
	static const vec_t v06251[] = {
		{ 0.0, { 3988.31022699, 5498.96657235, 0.90055879 }, { -3.290032738, 2.357652820, 6.496623475 } },
		{ 120.0, { -3935.69800083, 409.10980837, 5471.33577327 }, { -3.374784183, -6.635211043, -1.942056221 } },
	};
	check_vectors("06251", "1 06251U 62025E   06176.82412014  .00008885  00000-0  12808-3 0  3985",
		      "2 06251  58.0579  54.0425 0030035 139.1568 221.1854 15.56387291  6374", v06251, 2);
	static const vec_t v28057[] = {
		{ 0.0, { -2715.28237486, -6619.26436889, -0.01341443 }, { -1.008587273, 0.422782003, 7.385272942 } },
	};
	check_vectors("28057", "1 28057U 03049A   06177.78615833  .00000060  00000-0  35940-4 0  1836",
		      "2 28057  98.4283 247.6961 0000884  88.1964 271.9322 14.35478080140550", v28057, 1);
	/* A geostationary set is deep space: refused */
	sgp4_elements_t e;
	sgp4_t s;
	parse("1 28626U 05008A   06176.46683397 -.00000205  00000-0  10000-3 0  2190",
	      "2 28626   0.0019 286.9433 0000335  13.7918  55.6504  1.00270176  4891", &e);
	CHECK(sgp4_init(&s, &e) == SGP4_DEEP);
}

#define MU 398600.8

static double norm(const double *a) { return sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

/* Two-body (Kepler) propagation by universal variables would be long: a fourth-order Runge-Kutta of r'' = -mu r/r^3
 * over 5 minutes in 1-second steps is exact enough to compare with */
static void twobody(double r[3], double v[3], double seconds)
{
	for (int i = 0; i < (int)seconds; i++) {
		double k[4][6], y[6], s[6];
		memcpy(y, r, sizeof(double) * 3);
		memcpy(y + 3, v, sizeof(double) * 3);
		for (int j = 0; j < 4; j++) {
			double h = j == 0 ? 0 : j == 3 ? 1.0 : 0.5;
			for (int m = 0; m < 6; m++)
				s[m] = y[m] + (j ? h * k[j - 1][m] : 0);
			double rr = norm(s), f = -MU / (rr * rr * rr);
			for (int m = 0; m < 3; m++) {
				k[j][m] = s[m + 3];
				k[j][m + 3] = f * s[m];
			}
		}
		for (int m = 0; m < 6; m++)
			y[m] += (k[0][m] + 2 * k[1][m] + 2 * k[2][m] + k[3][m]) / 6.0;
		memcpy(r, y, sizeof(double) * 3);
		memcpy(v, y + 3, sizeof(double) * 3);
	}
}

static void physics(void)
{
	const sat_info_t *iss = &sat_table[0];
	CHECK(iss->catnum == 25544u);
	sgp4_t s;
	CHECK(sgp4_init(&s, &iss->el) == SGP4_OK);
	/* Period: successive ascending node crossings (z from - to +) */
	double r[3], v[3], last_z = 0, cross[3];
	int nc = 0;
	double hmin = 1e9, hmax = 0, emin = 1e9, emax = -1e9;
	for (double t = 0; t < 400.0 && nc < 3; t += 0.05) {
		CHECK(sgp4_run(&s, t, r, v) == SGP4_OK);
		if (t > 0 && last_z < 0 && r[2] >= 0)
			cross[nc++] = t - 0.05 * r[2] / (r[2] - last_z);
		last_z = r[2];
		double h = norm(r) - 6378.137, en = norm(v) * norm(v) / 2.0 - MU / norm(r);
		hmin = fmin(hmin, h);
		hmax = fmax(hmax, h);
		emin = fmin(emin, en);
		emax = fmax(emax, en);
	}
	CHECK(nc == 3);
	double period = (cross[2] - cross[0]) / 2.0, kepler = 1440.0 / 15.5012;
	printf("  ISS-like: nodal period %.2f min (mean motion %.2f), height %.0f..%.0f km, energy %.4f..%.4f km2/s2\n",
	       period, kepler, hmin, hmax, emin, emax);
	CHECK(fabs(period - kepler) < 0.5);		/* J2 moves the node: under half a minute */
	CHECK(hmin > 380 && hmax < 450);
	CHECK((emax - emin) / fabs(emin) < 2e-3);	/* J2 short-period terms only */
	/* Two-body over 5 minutes from the SGP4 state: within a few km (J2 bends it slightly) */
	double r0[3], v0[3], r5[3], v5[3];
	CHECK(sgp4_run(&s, 600.0, r0, v0) == SGP4_OK);
	CHECK(sgp4_run(&s, 605.0, r5, v5) == SGP4_OK);
	twobody(r0, v0, 300.0);
	double d[3] = { r0[0] - r5[0], r0[1] - r5[1], r0[2] - r5[2] };
	printf("  ISS-like: two-body vs SGP4 after 5 min: %.2f km\n", norm(d));
	CHECK(norm(d) < 5.0);
	/* The range rate is the range's derivative (the earth's rotation in the velocity included) */
	sat_obs_t o;
	sat_observer(399334000, 328597000, 900, &o);	/* Ankara */
	double worst = 0;
	for (int i = 0; i < 200; i++) {
		double t = iss->el.epoch + i * 431.0;
		sat_look_t a, b, c;
		CHECK(sat_eval(&s, iss, t - 1.0, &o, &a) == SGP4_OK);
		CHECK(sat_eval(&s, iss, t, &o, &b) == SGP4_OK);
		CHECK(sat_eval(&s, iss, t + 1.0, &o, &c) == SGP4_OK);
		worst = fmax(worst, fabs((c.range_km - a.range_km) / 2.0 - b.rate_kms));
		CHECK(b.az >= 0.0f && b.az < 360.0f && b.el >= -90.0f && b.el <= 90.0f);
		CHECK(b.rate_kms > -8.0f && b.rate_kms < 8.0f);
	}
	printf("  range rate vs d(range)/dt: worst %.4f km/s\n", worst);
	CHECK(worst < 0.01);
}

static void times(void)
{
	/* GMST at J2000.0 (2000-01-01 12:00 UT1): 280.46061837 deg; Vallado example 3-5, 1992-08-20 12:14 UT1:
	 * 152.578787886 deg */
	CHECK(fabs(sat_gmst(43200.0) * 180.0 / PI - 280.46061837) < 1e-6);
	double t = (double)(timegm(&(struct tm){ .tm_year = 92, .tm_mon = 7, .tm_mday = 20, .tm_hour = 12, .tm_min = 14 }) - 946684800);
	CHECK(fabs(sat_gmst(t) * 180.0 / PI - 152.578787886) < 1e-6);
	/* The GPS date and time against the C library, 2000 .. 2067 (32-bit seconds) */
	for (int y = 2000; y < 2068; y++)
		for (int m = 1; m <= 12; m++)
			for (int d = 1; d <= 28; d += 9) {
				struct tm tm = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_hour = 23, .tm_min = 59, .tm_sec = 58 };
				int32_t want = (int32_t)(timegm(&tm) - 946684800);
				uint32_t ddmmyy = (uint32_t)(d * 10000 + m * 100 + y % 100);
				CHECK(sat_s2000(ddmmyy, 235958u) == want);
			}
	CHECK(sat_s2000(10100u, 0u) == 0);
	CHECK(sat_s2000(290224u, 120000u) == (int32_t)(timegm(&(struct tm){ .tm_year = 124, .tm_mon = 1, .tm_mday = 29, .tm_hour = 12 }) - 946684800));
	CHECK(sat_s2000(0u, 0u) == -1 && sat_s2000(10100u, 250000u) == -1 && sat_s2000(10168u, 0u) == -1);
}

/* Brute force: every second for the span, the passes above the horizon */
typedef struct {
	int32_t aos, los;
	float max_el;
} brute_t;

static int brute(const sgp4_t *s, const sat_info_t *info, const sat_obs_t *o, int32_t from, int32_t span, brute_t *out, int max)
{
	int n = 0;
	bool up = false;
	for (int32_t t = from; t < from + span; t++) {
		sat_look_t l;
		if (sat_eval(s, info, (double)t, o, &l) != SGP4_OK)
			break;
		if (l.el >= 0.0f && !up) {
			if (n == max)
				break;
			out[n].aos = t;
			out[n].max_el = l.el;
			up = true;
		}
		if (up && l.el > out[n].max_el)
			out[n].max_el = l.el;
		if (l.el < 0.0f && up) {
			out[n++].los = t - 1;
			up = false;
		}
	}
	return n;
}

static void passes(void)
{
	sat_obs_t o;
	sat_observer(399334000, 328597000, 900, &o);	/* Ankara */
	int total = 0, evals = 0, searches = 0, worst_evals = 0;
	float worst_el = 0;
	int32_t worst_t = 0;
	for (uint8_t i = 0; i < sat_count; i++) {
		const sat_info_t *info = &sat_table[i];
		sgp4_t s;
		CHECK(sgp4_init(&s, &info->el) == SGP4_OK);
		int32_t from = (int32_t)info->el.epoch + 3600;
		brute_t want[40];
		int n = brute(&s, info, &o, from, 3 * 86400, want, 40);
		CHECK(n >= 6);
		/* From the end of each pass found (and once from the middle of a pass), the next pass: it must be one the
		 * brute force has, and every pass skipped on the way must be one under 20 s (too short to be sure of) */
		int32_t t = from;
		for (int round = 0; round < 60; round++) {
			int k = 0;
			while (k < n && want[k].los < t)
				k++;
			if (k >= n - 1)
				break;		/* the last one may run past the brute force's span */
			int32_t start = round == 2 ? (want[k].aos + want[k].los) / 2 : t;
			sat_search_t ss;
			sat_search_start(&ss, start);
			int e = 1;
			while (!sat_search_step(&ss, &s, info, &o))
				e++;
			evals += e;
			searches++;
			worst_evals = e > worst_evals ? e : worst_evals;
			CHECK(ss.out.state == SAT_PASS_OK);
			if (ss.out.state != SAT_PASS_OK)
				break;
			int j = k;
			while (j < n && abs(ss.out.aos - want[j].aos) > 3 && ss.out.aos > want[j].aos) {
				if (want[j].los - want[j].aos >= 20)
					printf("  %s: missed a %d s pass\n", info->name, (int)(want[j].los - want[j].aos));
				CHECK(want[j].los - want[j].aos < 20);
				j++;
			}
			CHECK(j < n);
			if (j >= n)
				break;
			int32_t da = ss.out.aos - want[j].aos, dl = ss.out.los - want[j].los;
			if (abs(da) > 3 || abs(dl) > 3)
				printf("  %s pass %d: rise %+d s, set %+d s off\n", info->name, j, (int)da, (int)dl);
			CHECK(abs(da) <= 3 && abs(dl) <= 3);
			float de = fabsf((float)ss.out.max_el - want[j].max_el);
			worst_el = de > worst_el ? de : worst_el;
			worst_t = abs(da) > worst_t ? abs(da) : worst_t;
			worst_t = abs(dl) > worst_t ? abs(dl) : worst_t;
			CHECK(de <= 1.0f);
			total++;
			t = ss.out.los + 1;
		}
	}
	printf("  pass search: %d passes as the brute force has them (rise / set within %d s, highest within %.1f deg), "
	       "%d propagations a search on average, %d at most\n", total, (int)worst_t, worst_el, evals / searches, worst_evals);
	CHECK(total >= 20);
	CHECK(worst_evals < 400);
}

static void doppler(void)
{
	/* Coming closer at 7 km/s: the UHF downlink is heard 10.2 kHz high, the VHF uplink sent 3.4 kHz low */
	CHECK(sat_doppler_rx(437800000u, -7.0f) == 10222);
	CHECK(sat_doppler_tx(145990000u, -7.0f) == -3409);
	CHECK(sat_doppler_rx(437800000u, 7.0f) == -10222);
	CHECK(sat_doppler_tx(145990000u, 7.0f) == 3409);
	CHECK(sat_doppler_rx(145800000u, 0.0f) == 0);
	CHECK(sat_step_hz(145990000u) == 1000u && sat_step_hz(437800000u) == 2500u);
	CHECK(sat_tune_hz(437800000u, 10223) == 437810000u);
	CHECK(sat_tune_hz(437800000u, 1249) == 437800000u && sat_tune_hz(437800000u, 1250) == 437802500u);
	CHECK(sat_tune_hz(437800000u, -1250) == 437797500u && sat_tune_hz(437800000u, -1249) == 437800000u);
	CHECK(sat_tune_hz(145990000u, -3409) == 145987000u);
}

static void table(void)
{
	CHECK(sat_count >= 1 && sat_count <= SAT_MAX);
	CHECK(sat_tle_note[0] != 0);
	for (uint8_t i = 0; i < sat_count; i++) {
		const sat_info_t *t = &sat_table[i];
		sgp4_t s;
		double r[3], v[3];
		CHECK(t->name[0] && strlen(t->name) <= SAT_NAME_LEN);
		CHECK(sgp4_init(&s, &t->el) == SGP4_OK);
		for (double m = 0; m <= 30 * 1440.0; m += 360.0)
			CHECK(sgp4_run(&s, m, r, v) == SGP4_OK);
		bool vhf_d = t->down_hz >= 144000000u && t->down_hz <= 148000000u;
		bool uhf_d = t->down_hz >= 430000000u && t->down_hz <= 440000000u;
		bool vhf_u = t->up_hz >= 144000000u && t->up_hz <= 148000000u;
		bool uhf_u = t->up_hz >= 430000000u && t->up_hz <= 440000000u;
		CHECK((vhf_d || uhf_d) && (vhf_u || uhf_u));
		CHECK(t->ctcss10 == 0 || (t->ctcss10 >= 670 && t->ctcss10 <= 2541));
	}
}

int main(void)
{
	sgp4_vectors();
	physics();
	times();
	passes();
	doppler();
	table();
	if (failures) {
		printf("test_sat: %d failures\n", failures);
		return 1;
	}
	printf("test_sat: ok\n");
	return 0;
}
