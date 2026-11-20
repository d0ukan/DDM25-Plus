/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus locator calculator (CONFIG_LOCATOR, locator.h), a page of Tools > Go-To (# there). # types a 4 or 6
 * character Maidenhead locator with the radio's text entry (the keypad letters; a held digit is the digit); the page
 * shows its centre's latitude / longitude and the distance and bearing from where we are (aprs_position_now: the GPS
 * fix, else the computer's, else the fixed position, as Tools > Repeaters), and our own locator. OK saves the centre
 * as a waypoint named after the locator (Go-To's list, RAM only). Back returns to Go-To. Installed as
 * core/ui/ui_locator.c. RAM: the typed text (7 B). */
#include "config.h"
#if CONFIG_LOCATOR
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_svc.h"
#include "fmt.h"
#include "i18n.h"
#include "gfx.h"
#include "aprs/aprs.h"
#include "ui/nav.h"
#include "ui/locator.h"

static char loc_text[LOC_LEN_MAX + 1];	/* as typed */

/* "41.7292 N" / "72.7083 W" */
static void coord(sbuf_t *b, int32_t e7, char pos, char neg)
{
	sb_degrees(b, e7 < 0 ? -e7 : e7);
	sb_chr(b, ' ');
	sb_chr(b, e7 < 0 ? neg : pos);
}

static void loc_draw(void)
{
	char buf[40], norm[LOC_LEN_MAX + 1];
	sbuf_t b;
	int32_t lat = 0, lon = 0, here_lat, here_lon;
	uint32_t err;
	bool dmr;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Locator"), "") + 4;
	const font_t *s = ui_layout.small, *m = ui_layout.medium, *big = ui_layout.large;
	int w = gfx_width();
	bool ok = loc_parse(loc_text, &lat, &lon, norm);
	const char *shown = ok ? norm : loc_text[0] ? loc_text : "------";
	gfx_text(big, (w - gfx_text_width(big, shown)) / 2, y, shown, ok ? ui_col.fg : ui_col.dim);
	y += big->h + 6;
	if (!ok) {
		gfx_text(s, 0, y, loc_text[0] ? TR("Not a locator: like KM69 or KM69kw") : TR("# types a locator"),
			 loc_text[0] ? ui_col.warn : ui_col.dim);
		y += s->h + 4;
	} else {
		gfx_text(s, 0, y, TR("Centre"), ui_col.dim);
		y += s->h + 1;
		sb_init(&b, buf, sizeof buf);
		coord(&b, lat, 'N', 'S');
		sb_str(&b, "  ");
		coord(&b, lon, 'E', 'W');
		gfx_text(s, 0, y, buf, ui_col.fg);
		y += s->h + 4;
	}
	bool here = aprs_position_now(&here_lat, &here_lon, &err, &dmr);
	if (ok && here) {
		nav_way_t way;
		nav_way(here_lat, here_lon, lat, lon, 0xFFFFu, 0xFFFFu, &way);
		sb_init(&b, buf, sizeof buf);
		sb_range(&b, way.meters, way.bearing);
		sb_str(&b, "  ");
		sb_u32(&b, way.bearing, 1);
		sb_str(&b, GLYPH_DEGREE);
		gfx_text(m, 0, y, buf, ui_col.rx);
		y += m->h + 4;
	}
	sb_init(&b, buf, sizeof buf);
	if (here) {
		char mine[LOC_LEN_MAX + 1];
		loc_encode(here_lat, here_lon, mine);
		sb_str(&b, TR("Here "));
		sb_str(&b, mine);
	} else {
		sb_str(&b, TR("No position: GPS or Settings > Location"));
	}
	gfx_text(s, 0, y, buf, here ? ui_col.dim : ui_col.warn);
	ui_draw_bottom(&ui_st, ok ? TR(TEXT_ALT_OPEN "# type  OK save waypoint  Back" TEXT_ALT_NEXT "# type  OK save" TEXT_ALT_CLOSE)
				  : TR("# type  Back"));
	ui_draw_banner(&ui_st);
}

static void loc_typed(void)
{
	int32_t lat, lon;
	char norm[LOC_LEN_MAX + 1];
	if (loc_parse(loc_text, &lat, &lon, norm))
		memcpy(loc_text, norm, sizeof norm);
	else if (loc_text[0])
		ui_toast(TR("Not a locator: like KM69 or KM69kw"));
	ui_dirty();
}

static bool loc_key(const input_event_t *ev)
{
	if (ev->gesture != GESTURE_PRESS)
		return false;
	if (ev->action == ACT_CONTEXT) {
		ui_text_edit(loc_text, LOC_LEN_MAX, loc_typed, 2);	/* arg 2: titled "Locator" */
		return true;
	}
	if (ev->action == ACT_SELECT) {
		int32_t lat, lon;
		char norm[LOC_LEN_MAX + 1];
		if (!loc_parse(loc_text, &lat, &lon, norm)) {
			ui_toast(TR("# types a locator"));
		} else {
			nav_save(lat, lon);
			nav_rename(0, norm);
			ui_toast(TR("Saved as a waypoint"));
		}
		ui_dirty();
		return true;
	}
	return false;
}

static void loc_enter(void)
{
	ui_dirty();
}

const ui_scr_def_t ui_locator_screen = {
	.name = "locator", .flags = SF_FIELD, .key = loc_key, .draw = loc_draw, .enter = loc_enter,
};
#endif
