/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus offline Turkey map renderer core (map.h). Integer only: CK803S has no FPU. Units are 0.001 degree
 * east / north of the data's origin (turkey_map_data.h); the province lookup works in 0.0001 degree. */
#include "config.h"
#if CONFIG_MAP
#include <stddef.h>
#include <string.h>
#include "map.h"
#include "turkey_map_data.h"
#include "turkey_district_data.h"

/* Units per pixel x 16 (latitude), zoom 0..6: 0.1, 0.05, 0.025, 0.0125, 0.00625, 0.003125, 0.0015625 degree per
 * pixel */
static const uint16_t upp_q4[MAP_ZOOMS] = { 1600, 800, 400, 200, 100, 50, 25 };
/* cos(latitude) Q15 at 30 .. 50 degrees */
static const uint16_t cos_q15[21] = { 28378, 28088, 27789, 27482, 27166, 26842, 26510, 26170, 25822, 25466, 25102,
	24730, 24351, 23965, 23571, 23170, 22763, 22348, 21926, 21498, 21063 };

#define MARGIN		2000	/* the centre may leave the data's bounds by 2 degrees */
#define SPAN		60000	/* farther than this from the centre (units) is clamped before projecting */
#define M_PER_UNIT_X100	11132u	/* metres per 0.001 degree of latitude, x 100 */

static int32_t floor_div(int32_t a, int32_t b)	/* b > 0 */
{
	int32_t q = a / b;
	return (a % b != 0 && a < 0) ? q - 1 : q;
}

static int32_t ceil_div(int32_t a, int32_t b)	/* b > 0 */
{
	int32_t q = a / b;
	return (a % b != 0 && a > 0) ? q + 1 : q;
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* cos(latitude) Q15 for a latitude in units, linear between whole degrees */
static int32_t cos_at(int32_t uy)
{
	int32_t milli = clamp32(uy + (TURKEY_MAP_LAT0_E7 / TURKEY_MAP_UNIT_E7), 30000, 49999) - 30000;
	int32_t i = milli / 1000, f = milli % 1000;
	return (int32_t)cos_q15[i] + ((int32_t)cos_q15[i + 1] - (int32_t)cos_q15[i]) * f / 1000;
}

static void view_scale(map_view_t *v)
{
	v->cx = clamp32(v->cx, TURKEY_MAP_XMIN - MARGIN, TURKEY_MAP_XMAX + MARGIN);
	v->cy = clamp32(v->cy, TURKEY_MAP_YMIN - MARGIN, TURKEY_MAP_YMAX + MARGIN);
	v->ky = (int32_t)((16ul << 16) / upp_q4[v->zoom]);
	v->kx = v->ky * cos_at(v->cy) / 32768;
}

static int32_t ux_of(int32_t lon_e7)
{
	return floor_div(lon_e7 - TURKEY_MAP_LON0_E7, TURKEY_MAP_UNIT_E7);
}

static int32_t uy_of(int32_t lat_e7)
{
	return floor_div(lat_e7 - TURKEY_MAP_LAT0_E7, TURKEY_MAP_UNIT_E7);
}

/* SPAN x k overflows int32 from zoom 6 on: the product is 64-bit (the shift floors: arithmetic on GCC / clang) */
static int32_t px_x(const map_view_t *v, int32_t ux)
{
	return v->x + v->w / 2 + (int32_t)(((int64_t)clamp32(ux - v->cx, -SPAN, SPAN) * v->kx) >> 16);
}

static int32_t px_y(const map_view_t *v, int32_t uy)
{
	return v->y + v->h / 2 + (int32_t)(((int64_t)clamp32(v->cy - uy, -SPAN, SPAN) * v->ky) >> 16);
}

void map_view_init(map_view_t *v, int x, int y, int w, int h)
{
	v->x = (int16_t)x;
	v->y = (int16_t)y;
	v->w = (int16_t)w;
	v->h = (int16_t)h;
	v->cx = (TURKEY_MAP_XMIN + TURKEY_MAP_XMAX) / 2;
	v->cy = (TURKEY_MAP_YMIN + TURKEY_MAP_YMAX) / 2;
	v->zoom = 0;
	view_scale(v);
}

void map_view_set(map_view_t *v, int32_t lat_e7, int32_t lon_e7, uint8_t zoom)
{
	v->cx = ux_of(lon_e7);
	v->cy = uy_of(lat_e7);
	v->zoom = zoom < MAP_ZOOMS ? zoom : (uint8_t)(MAP_ZOOMS - 1u);
	view_scale(v);
}

void map_view_zoom(map_view_t *v, int dir)
{
	int z = (int)v->zoom + (dir > 0 ? 1 : dir < 0 ? -1 : 0);
	v->zoom = (uint8_t)(z < 0 ? 0 : z >= (int)MAP_ZOOMS ? (int)MAP_ZOOMS - 1 : z);
	view_scale(v);
}

void map_view_pan(map_view_t *v, int dx_px, int dy_px)
{
	v->cx += floor_div((int32_t)dx_px * 65536, v->kx);
	v->cy -= floor_div((int32_t)dy_px * 65536, v->ky);
	view_scale(v);
}

void map_view_center(const map_view_t *v, int32_t *lat_e7, int32_t *lon_e7)
{
	*lat_e7 = TURKEY_MAP_LAT0_E7 + v->cy * TURKEY_MAP_UNIT_E7;
	*lon_e7 = TURKEY_MAP_LON0_E7 + v->cx * TURKEY_MAP_UNIT_E7;
}

uint32_t map_view_width_m(const map_view_t *v)
{
	uint32_t units = (uint32_t)v->w * 65536u / (uint32_t)v->ky;	/* latitude units: true distance */
	return units * M_PER_UNIT_X100 / 100u;
}

bool map_project(const map_view_t *v, int32_t lat_e7, int32_t lon_e7, int *x, int *y)
{
	int32_t px = px_x(v, ux_of(lon_e7)), py = px_y(v, uy_of(lat_e7));
	*x = (int)px;
	*y = (int)py;
	return px >= v->x && px < v->x + v->w && py >= v->y && py < v->y + v->h;
}

void map_unproject(const map_view_t *v, int x, int y, int32_t *lat_e7, int32_t *lon_e7)
{
	/* ceil: the smallest unit whose floor projection is this pixel, so project(unproject(p)) == p */
	int32_t ux = v->cx + ceil_div((int32_t)(x - v->x - v->w / 2) * 65536, v->kx);
	int32_t uy = v->cy - ceil_div((int32_t)(y - v->y - v->h / 2) * 65536, v->ky);
	*lat_e7 = TURKEY_MAP_LAT0_E7 + uy * TURKEY_MAP_UNIT_E7;
	*lon_e7 = TURKEY_MAP_LON0_E7 + ux * TURKEY_MAP_UNIT_E7;
}

enum { OUT_L = 1, OUT_R = 2, OUT_T = 4, OUT_B = 8 };

static unsigned outcode(int32_t x, int32_t y, int32_t x0r, int32_t y0r, int32_t x1r, int32_t y1r)
{
	return (x < x0r ? OUT_L : x > x1r ? OUT_R : 0u) | (y < y0r ? OUT_T : y > y1r ? OUT_B : 0u);
}

bool map_clip(int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1, int32_t x0r, int32_t y0r, int32_t x1r, int32_t y1r)
{
	unsigned c0 = outcode(*x0, *y0, x0r, y0r, x1r, y1r), c1 = outcode(*x1, *y1, x0r, y0r, x1r, y1r);
	for (int guard = 0; guard < 8; guard++) {
		if (!(c0 | c1))
			return true;
		if (c0 & c1)
			return false;
		unsigned c = c0 ? c0 : c1;
		int32_t dx = *x1 - *x0, dy = *y1 - *y0, x, y;
		if (c & OUT_T) {
			y = y0r;
			x = *x0 + dx * (y0r - *y0) / dy;
		} else if (c & OUT_B) {
			y = y1r;
			x = *x0 + dx * (y1r - *y0) / dy;
		} else if (c & OUT_L) {
			x = x0r;
			y = *y0 + dy * (x0r - *x0) / dx;
		} else {
			x = x1r;
			y = *y0 + dy * (x1r - *x0) / dx;
		}
		if (c == c0) {
			*x0 = x;
			*y0 = y;
			c0 = outcode(x, y, x0r, y0r, x1r, y1r);
		} else {
			*x1 = x;
			*y1 = y;
			c1 = outcode(x, y, x0r, y0r, x1r, y1r);
		}
	}
	return false;
}

uint32_t map_draw(const map_view_t *v, map_line_fn fn, void *ctx)
{
	uint32_t n = 0;
	int32_t rx0 = v->x, ry0 = v->y, rx1 = v->x + v->w - 1, ry1 = v->y + v->h - 1;
	for (uint8_t p = 0; p < TURKEY_MAP_PROVINCES; p++) {
		const turkey_map_province_t *pr = &turkey_map_provinces[p];
		if (px_x(v, pr->max.x) < rx0 || px_x(v, pr->min.x) > rx1 || px_y(v, pr->min.y) < ry0 ||
		    px_y(v, pr->max.y) > ry1)
			continue;
		for (uint16_t r = pr->first_ring; r < pr->first_ring + pr->ring_count; r++) {
			const turkey_map_ring_t *ring = &turkey_map_rings[r];
			int32_t ux = ring->start.x, uy = ring->start.y;
			int32_t lx = px_x(v, ux), ly = px_y(v, uy);
			for (uint16_t i = ring->first; i < ring->first + ring->count; i++) {
				ux += turkey_map_steps[i][0];
				uy += turkey_map_steps[i][1];
				int32_t x = px_x(v, ux), y = px_y(v, uy);
				if (x == lx && y == ly)
					continue;
				int32_t a = lx, b = ly, c = x, d = y;
				if (map_clip(&a, &b, &c, &d, rx0, ry0, rx1, ry1)) {
					fn(ctx, (int)a, (int)b, (int)c, (int)d, p);
					n++;
				}
				lx = x;
				ly = y;
			}
		}
	}
	return n;
}

bool map_label(const map_view_t *v, uint8_t province, int *x, int *y)
{
	if (province >= TURKEY_MAP_PROVINCES)
		return false;
	const turkey_map_pt_t *l = &turkey_map_provinces[province].label;
	int32_t px = px_x(v, l->x), py = px_y(v, l->y);
	*x = (int)px;
	*y = (int)py;
	return px >= v->x && px < v->x + v->w && py >= v->y && py < v->y + v->h;
}

/* Even-odd over all of a province's rings, in 0.0001 degree (steps are at most 1270 of those: no overflow) */
static bool inside(const turkey_map_province_t *pr, int32_t px, int32_t py)
{
	if (px < pr->min.x * 10 || px > pr->max.x * 10 || py < pr->min.y * 10 || py > pr->max.y * 10)
		return false;
	bool in = false;
	for (uint16_t r = pr->first_ring; r < pr->first_ring + pr->ring_count; r++) {
		const turkey_map_ring_t *ring = &turkey_map_rings[r];
		int32_t ax = ring->start.x * 10, ay = ring->start.y * 10;
		for (uint16_t i = ring->first; i < ring->first + ring->count; i++) {
			int32_t bx = ax + turkey_map_steps[i][0] * 10, by = ay + turkey_map_steps[i][1] * 10;
			if ((ay > py) != (by > py)) {
				int32_t lhs = (px - ax) * (by - ay), rhs = (py - ay) * (bx - ax);
				if (by > ay ? lhs < rhs : lhs > rhs)
					in = !in;
			}
			ax = bx;
			ay = by;
		}
	}
	return in;
}

int map_province_at(int32_t lat_e7, int32_t lon_e7)
{
	int32_t px = floor_div(lon_e7 - TURKEY_MAP_LON0_E7, 1000), py = floor_div(lat_e7 - TURKEY_MAP_LAT0_E7, 1000);
	for (uint8_t p = 0; p < TURKEY_MAP_PROVINCES; p++)
		if (inside(&turkey_map_provinces[p], px, py))
			return p;
	return MAP_NONE;
}

int map_province_near(int32_t lat_e7, int32_t lon_e7, uint32_t max_m)
{
	int at = map_province_at(lat_e7, lon_e7);
	if (at != MAP_NONE || max_m == 0)
		return at;
	int32_t px = floor_div(lon_e7 - TURKEY_MAP_LON0_E7, 1000), py = floor_div(lat_e7 - TURKEY_MAP_LAT0_E7, 1000);
	int32_t cosq = cos_at(py / 10);
	int32_t m = (int32_t)(max_m > 50000u ? 50000u : max_m) * 1000 / (int32_t)M_PER_UNIT_X100;	/* 0.0001 deg */
	int64_t best = (int64_t)m * m;
	int found = MAP_NONE;
	for (uint8_t p = 0; p < TURKEY_MAP_PROVINCES; p++) {
		const turkey_map_province_t *pr = &turkey_map_provinces[p];
		int32_t mx = m * 32768 / cosq + 1;	/* the margin in longitude */
		if (px < pr->min.x * 10 - mx || px > pr->max.x * 10 + mx || py < pr->min.y * 10 - m ||
		    py > pr->max.y * 10 + m)
			continue;
		for (uint16_t r = pr->first_ring; r < pr->first_ring + pr->ring_count; r++) {
			const turkey_map_ring_t *ring = &turkey_map_rings[r];
			int32_t ax = ring->start.x * 10, ay = ring->start.y * 10;
			for (uint16_t i = ring->first; i < ring->first + ring->count; i++) {
				int32_t bx = ax + turkey_map_steps[i][0] * 10, by = ay + turkey_map_steps[i][1] * 10;
				/* longitude scaled by cos(latitude): distances in latitude units */
				int64_t dx = (int64_t)(bx - ax) * cosq / 32768, dy = by - ay;
				int64_t wx = (int64_t)(px - ax) * cosq / 32768, wy = py - ay;
				int64_t t = wx * dx + wy * dy, dd = dx * dx + dy * dy, d2;
				if (t <= 0 || dd == 0) {
					d2 = wx * wx + wy * wy;
				} else if (t >= dd) {
					int64_t ex = wx - dx, ey = wy - dy;
					d2 = ex * ex + ey * ey;
				} else {
					d2 = wx * wx + wy * wy - t * t / dd;
				}
				if (d2 < best) {
					best = d2;
					found = p;
				}
				ax = bx;
				ay = by;
			}
		}
	}
	return found;
}

const char *map_province_name(uint8_t province)
{
	return province < TURKEY_MAP_PROVINCES ? &turkey_map_names[turkey_map_provinces[province].name] : "";
}

uint8_t map_province_plate(uint8_t province)
{
	return province < TURKEY_MAP_PROVINCES ? turkey_map_provinces[province].plate : 0u;
}

/* ---- Districts (turkey_district_data.h, GeoNames, CC BY 4.0) ---- */

uint16_t map_district_count(void)
{
	return (uint16_t)TURKEY_DISTRICTS;
}

uint8_t map_district_province(uint16_t d)
{
	if (d >= TURKEY_DISTRICTS)
		return 0xFFu;
	uint8_t lo = 0, hi = (uint8_t)(TURKEY_MAP_PROVINCES - 1u);	/* the last p with first[p] <= d */
	while (lo < hi) {
		uint8_t mid = (uint8_t)((lo + hi + 1u) / 2u);
		if (turkey_district_first[mid] <= d)
			lo = mid;
		else
			hi = (uint8_t)(mid - 1u);
	}
	return lo;
}

const char *map_district_name(uint16_t d)
{
	uint8_t p = map_district_province(d);
	if (p == 0xFFu)
		return "";
	const char *s = &turkey_district_names[turkey_district_name_first[p]];
	for (uint16_t i = turkey_district_first[p]; i < d; i++)
		s += strlen(s) + 1u;
	return s;
}

void map_district_pos(uint16_t d, int32_t *lat_e7, int32_t *lon_e7)
{
	const turkey_map_pt_t *q = &turkey_district_pos[d < TURKEY_DISTRICTS ? d : 0u];
	*lat_e7 = TURKEY_MAP_LAT0_E7 + q->y * TURKEY_MAP_UNIT_E7 + TURKEY_MAP_UNIT_E7 / 2;
	*lon_e7 = TURKEY_MAP_LON0_E7 + q->x * TURKEY_MAP_UNIT_E7 + TURKEY_MAP_UNIT_E7 / 2;
}

uint16_t map_districts_draw(const map_view_t *v, map_point_fn fn, void *ctx)
{
	uint16_t n = 0;
	int32_t rx0 = v->x, ry0 = v->y, rx1 = v->x + v->w - 1, ry1 = v->y + v->h - 1;
	for (uint8_t p = 0; p < TURKEY_MAP_PROVINCES; p++) {
		const turkey_map_province_t *pr = &turkey_map_provinces[p];
		/* a district centre lies inside (or within a few km of) its province: one pixel of slack */
		if (px_x(v, pr->max.x) < rx0 - 1 || px_x(v, pr->min.x) > rx1 + 1 || px_y(v, pr->min.y) < ry0 - 1 ||
		    px_y(v, pr->max.y) > ry1 + 1)
			continue;
		const char *name = &turkey_district_names[turkey_district_name_first[p]];
		for (uint16_t d = turkey_district_first[p]; d < turkey_district_first[p + 1u]; d++) {
			int32_t x = px_x(v, turkey_district_pos[d].x), y = px_y(v, turkey_district_pos[d].y);
			if (x >= rx0 && x <= rx1 && y >= ry0 && y <= ry1) {
				fn(ctx, (int)x, (int)y, d, name);
				n++;
			}
			name += strlen(name) + 1u;
		}
	}
	return n;
}

uint16_t map_district_near(int32_t lat_e7, int32_t lon_e7, int province, uint32_t max_m)
{
	int32_t ux = ux_of(lon_e7), uy = uy_of(lat_e7);
	if (ux < TURKEY_MAP_XMIN - MARGIN || ux > TURKEY_MAP_XMAX + MARGIN || uy < TURKEY_MAP_YMIN - MARGIN ||
	    uy > TURKEY_MAP_YMAX + MARGIN)
		return MAP_DISTRICT_NONE;
	int32_t cosq = cos_at(uy);
	int32_t m = (int32_t)(max_m > 100000u ? 100000u : max_m) * 100 / (int32_t)M_PER_UNIT_X100;	/* units */
	int32_t best = m * m;
	uint16_t found = MAP_DISTRICT_NONE;
	uint16_t d0 = 0, d1 = (uint16_t)TURKEY_DISTRICTS;
	if (province >= 0 && province < (int)TURKEY_MAP_PROVINCES) {
		d0 = turkey_district_first[province];
		d1 = turkey_district_first[province + 1];
	}
	for (uint16_t d = d0; d < d1; d++) {
		int32_t dx = (turkey_district_pos[d].x - ux) * cosq / 32768, dy = turkey_district_pos[d].y - uy;
		if (dx > m || dx < -m || dy > m || dy < -m)
			continue;
		int32_t d2 = dx * dx + dy * dy;
		if (d2 < best || (d2 == best && found == MAP_DISTRICT_NONE)) {
			best = d2;
			found = d;
		}
	}
	return found;
}

/* ---- Label room ---- */

void map_occ_init(map_occ_t *o, int x, int y, int w, int h)
{
	o->x = (int16_t)x;
	o->y = (int16_t)y;
	o->cw = (int16_t)((w + MAP_OCC_COLS - 1) / MAP_OCC_COLS);
	o->ch = (int16_t)((h + MAP_OCC_ROWS - 1) / MAP_OCC_ROWS);
	if (o->cw < 1)
		o->cw = 1;
	if (o->ch < 1)
		o->ch = 1;
	memset(o->row, 0, sizeof o->row);
}

bool map_occ_take(map_occ_t *o, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0 || x < o->x || y < o->y)
		return false;
	int c0 = (x - o->x) / o->cw, c1 = (x + w - 1 - o->x) / o->cw;
	int r0 = (y - o->y) / o->ch, r1 = (y + h - 1 - o->y) / o->ch;
	if (c1 >= (int)MAP_OCC_COLS || r1 >= (int)MAP_OCC_ROWS)
		return false;
	uint16_t mask = (uint16_t)(((1u << (c1 - c0 + 1)) - 1u) << c0);
	for (int r = r0; r <= r1; r++)
		if (o->row[r] & mask)
			return false;
	for (int r = r0; r <= r1; r++)
		o->row[r] |= mask;
	return true;
}
#endif
