/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus round 3 "misc" job: the UI hooks upstream's files call (tools/plus_features.py prepare_misc). Tools >
 * Airfields (CONFIG_AIRFIELDS, air/), Tools > Marine VHF (CONFIG_MARINE, marine/), the QSO log on Recent
 * (CONFIG_QSO_LOG, qso/), Tools > Battery's history (CONFIG_BATT_GRAPH, battery/). */
#ifndef PLUS_UI_MISC_H
#define PLUS_UI_MISC_H

#include <stdbool.h>
#include <stdint.h>
#include "radio/radio.h"
#include "hal_input.h"

/* ui_battgraph.c: the history's sample (ui_app_tick, any screen), its start over (ui_app_init), and whether Tools >
 * Battery opens the history (a radio with a battery) or upstream's page (a radio that measures its supply) */
void ui_battgraph_tick(const radio_view_t *v, uint32_t now_ms);
void ui_battgraph_reset(void);
bool ui_battgraph_wanted(void);

/* ui_qso.c: * on a Recent row (the QSO mark) */
bool ui_qso_recent_key(const input_event_t *ev);

#endif
