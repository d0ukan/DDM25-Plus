/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus priority channel and temporary skip, the UI (CONFIG_SCAN_PRIO, docs/plus.md "Priority channel and
 * temporary skip"; the radio core's part is radio_scanprio.inc):
 * - 0 on Home while a zone or VFO scan has stopped on a signal: that channel (frequency) is skipped until the scan
 *   stops (upstream's nuisance delete, which # 1 also gives). While the scan is still stepping 0 says there is nothing
 *   to skip yet (it would skip whichever channel the receiver passes);
 * - # while scanning: upstream's three (skip, stay here, always skip), then 4 Clear skips and 5 Set as priority
 *   (Priority off when the channel is the priority one);
 * - Settings > Talk & listen > Priority channel: Off or the channel's number; OK sets the channel on view (OK again:
 *   Off). The zone editor's channel actions set any channel of a zone;
 * - Home marks the priority channel's number with a P, and the status strip's SCAN / DUAL word reads "SCAN P" while a
 *   priority channel is set. */
#include "config.h"
#if CONFIG_SCAN_PRIO
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "i18n.h"
#include "radio/scan.h"
#include "radio/scanprio.h"

static uint32_t scx_word;	// the setting (UI_SET_SCAN_PRIO) as ui_apply_setting last gave it
static char scx_msg[40];	// a toast naming the channel
static char scx_strip[16];	// "SCAN P"

void ui_scx_apply(int32_t v)
{
	scx_word = (uint32_t)v;
}

uint16_t ui_scx_channel(void)
{
	return scanprio_number(scx_word);
}

bool ui_scx_marked(const radio_view_t *v)
{
	return !v->vfo && v->channel_number != 0 && v->channel_number == ui_scx_channel();
}

// The status strip's scan word, with " P" while a priority channel is set
const char *ui_scx_word(const char *word)
{
	if (ui_scx_channel() == 0)
		return word;
	sbuf_t b;
	sb_init(&b, scx_strip, sizeof scx_strip);
	sb_str(&b, word);
	sb_str(&b, " P");
	return scx_strip;
}

// Channel number (0: Off) of zone zi; says so with a toast. False: it couldn't be saved
bool ui_scx_set(uint16_t number, uint16_t zi, const char *name)
{
	int32_t w = (int32_t)scanprio_pack(number, zi);
	if (!ui_svc_setting_set(UI_SET_SCAN_PRIO, w)) {
		ui_toast(TR("Couldn't save: try again"));
		return false;
	}
	ui_apply_setting(UI_SET_SCAN_PRIO, w);
	ui_svc_apply(UI_SET_SCAN_PRIO, w);	// the radio core's copy
	if (w == 0) {
		ui_toast(TR("Priority channel off"));
		return true;
	}
	sbuf_t b;
	sb_init(&b, scx_msg, sizeof scx_msg);
	sb_str(&b, TR("Priority: "));
	sb_u32(&b, number, 1);
	if (name != NULL && name[0] != '\0') {
		sb_chr(&b, ' ');
		sb_str(&b, name);
	}
	ui_toast(scx_msg);
	return true;
}

// The channel on view becomes the priority channel, or (it is already) priority is off
static void scx_toggle_here(void)
{
	const radio_view_t *v = ui_view();
	if (ui_scx_marked(v)) {
		ui_scx_set(0, 0, NULL);
	} else if (v->vfo || v->channel_number == 0) {
		ui_toast(TR("Pick a channel first"));
	} else {
		ui_scx_set(v->channel_number, v->zone_index, v->ch.name);
	}
}

// Settings > Talk & listen > Priority channel: its value, and OK on it
void ui_scx_setting_value(char *out, uint8_t cap)
{
	uint16_t n = ui_scx_channel();
	if (n == 0) {
		str_copy(out, cap, TR("Off"));
		return;
	}
	sbuf_t b;
	sb_init(&b, out, cap);
	sb_u32(&b, n, 1);
}

void ui_scx_setting_select(void)
{
	if (ui_scx_channel() != 0)
		ui_scx_set(0, 0, NULL);
	else
		scx_toggle_here();
}

// A scan has stopped on something (a signal, or a carrier it probes): there is a channel to skip
static bool scx_stopped(const radio_view_t *v)
{
	return v->scan_phase == SCAN_PH_HOLD || v->scan_phase == SCAN_PH_RESUME || v->scan_phase == SCAN_PH_PROBE;
}

// Home while a zone or VFO scan runs: 0 skips the channel it stopped on (nothing else uses 0 first then: Find has no
// channel 0 and no frequency starts with it)
bool ui_scx_home_key(const input_event_t *ev)
{
	if (!ui_st.scanning || ui_st.entry || ev->action != ACT_DIGIT_0 || ev->gesture != GESTURE_PRESS)
		return false;
	if (!scx_stopped(ui_view()))
		ui_toast(TR("Nothing to skip yet"));
	else if (ui_svc_scan_skip())
		ui_toast(TR("Skipped until the scan stops"));
	return true;
}

// ======================= # while scanning: after upstream's three (ui_menus.c scan_screen) =======================

enum { SM_UNSKIP = 3, SM_PRIO };

void ui_scx_menu_item(uint16_t i, ui_item_t *it)
{
	const radio_view_t *v = ui_view();
	const char *name = i == SM_UNSKIP ? TRN("Clear skips") : ui_scx_marked(v) ? TRN("Priority off") : TRN("Set as priority");
	str_copy(it->label, sizeof it->label, tr(name));
	item_num_set(it, (uint16_t)(i + 1u));
	if (i == SM_PRIO && v->vfo)
		it->flags |= UI_ITEM_DIM;
	if (i == SM_UNSKIP && v->scan_skipped != 0) {
		sbuf_t b;
		sb_init(&b, it->value, sizeof it->value);
		sb_u32(&b, v->scan_skipped, 1);
	}
}

void ui_scx_menu_select(uint16_t i)
{
	ui_go_home();
	if (i == SM_UNSKIP)
		ui_toast(ui_svc_scan_unskip() ? TR("Skips cleared") : TR("Scan: not in this build"));
	else
		scx_toggle_here();
}
#endif
