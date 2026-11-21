/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus: one static buffer shared by the tool screens' copies of what they draw (docs/plus.md "RAM reclaim, round
 * 13"). Installed as core/ui/ui_scratch.{c,h}.
 *
 * Each of these screens refills its copy at the start of its own draw (the radio task's snapshot, or the message page
 * built again) and reads it only in that draw and in its key handler, which takes a fresh snapshot first. Only the
 * screen on top draws and gets keys, so one buffer serves all six: Tools > Spectrum (spectrum_view_t), DMR search,
 * P25 search, DMR trunk, P25 trunk, and the pager / ACARS message page; and the APRS station page (DDM32 Plus round
 * 14). UI task only.
 *
 * What must outlive a draw stays out of here, in each screen's own small statics (the span, the range, the find picked
 * with Up / Down, the page's scroll). The one reader outside its screen is the find save (ui_findsave.c): it reads the
 * P25 / DMR search list frozen as the save began, possibly a few seconds after the user left the screen. It asks
 * ui_scratch_mine first: when another screen has taken the buffer since, the list is gone and the save stops there
 * (each channel already written is whole). */
#ifndef PLUS_UI_SCRATCH_H
#define PLUS_UI_SCRATCH_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#if CONFIG_XBAND
#include "codeplug/codeplug.h"
#endif
#if CONFIG_SPECTRUM
#include "radio/spectrum.h"
#endif
#if CONFIG_DMR_SEARCH
#include "radio/dmrsearch.h"
#endif
#if CONFIG_P25_SEARCH
#include "radio/p25search.h"
#endif
#if CONFIG_DMR_TRUNK
#include "radio/dmr_csbk.h"
#include "radio/dmrtrunk.h"
#endif
#if CONFIG_P25_TRUNK
#include "radio/p25trunk.h"
#endif
#if CONFIG_MSG_RX
#include "radio/radio_msg.h"
#define UI_SCRATCH_MSG_PAGE	(RADIO_MSG_TEXT + 104u)	/* one message's page (ui_msg.c) */
#endif
#if CONFIG_APRS_STATION
#include "aprs/aprs.h"
/* DDM32 Plus APRS station page (ui_aprsx.c): the station, a message of the inbox, a line and its cut copy */
typedef struct {
	aprs_station_t st;
	aprs_msg_t msg;
	char line[48], fit[48];	/* a line of the 240 px screen is about 26 small characters */
} ui_scratch_aprsx_t;
#endif

/* Who filled the buffer last */
enum { UI_SCRATCH_NONE, UI_SCRATCH_SPECTRUM, UI_SCRATCH_DMRS, UI_SCRATCH_P25S, UI_SCRATCH_DMRTK, UI_SCRATCH_P25TK,
       UI_SCRATCH_MSG, UI_SCRATCH_APRSX, UI_SCRATCH_XBAND,
       UI_SCRATCH_ALLS };	/* DDM32 Plus all digital search: its own tag on the DMR search's copy (dmrs_view_t) */

typedef union {
#if CONFIG_SPECTRUM
	spectrum_view_t spectrum;
#endif
#if CONFIG_DMR_SEARCH
	dmrs_view_t dmrs;
#endif
#if CONFIG_P25_SEARCH
	p25s_view_t p25s;
#endif
#if CONFIG_DMR_TRUNK
	dmrtk_view_t dmrtk;
#endif
#if CONFIG_P25_TRUNK
	p25tk_view_t p25tk;
#endif
#if CONFIG_MSG_RX
	char msg_page[UI_SCRATCH_MSG_PAGE];
#endif
#if CONFIG_APRS_STATION
	ui_scratch_aprsx_t aprsx;
#endif
#if CONFIG_XBAND
	struct {		/* DDM32 Plus Tools > Cross-band's picker (ui_xband.c): a zone, one of its channels */
		zone_t z;
		channel_t ch;
	} xband;
#endif
	uint32_t none;
} ui_scratch_t;

extern ui_scratch_t ui_scratch;

/* The buffer is owner's from now on (what another screen left there is gone); returns it */
ui_scratch_t *ui_scratch_take(uint8_t owner);
/* Nobody has taken the buffer since owner did */
bool ui_scratch_mine(uint8_t owner);

#endif
