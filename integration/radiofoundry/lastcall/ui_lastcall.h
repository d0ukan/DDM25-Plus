/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus Tools > Last call, the list of the last calls (ui_lastcall.c, lastcall.h). Installed as
 * core/ui/ui_lastcall.h. */
#ifndef PLUS_UI_LASTCALL_H
#define PLUS_UI_LASTCALL_H

#include <stdbool.h>
#include "config.h"
#include "ui/ui_nav.h"
#include "ui/lastcall.h"

/* ui_app.c's tick, before it feeds Recent: the view the calls below were heard on (the watched letter, DMR's colour
 * code and slot) */
void ui_lastcall_view(const radio_view_t *v);
/* A Recent entry: new_call when recent_add takes it in, false while that call goes on (a DMR or P25 call heard again) */
void ui_lastcall_recent(const ui_recent_t *r, bool new_call);
const lastcall_list_t *ui_lastcall_list(void);
void ui_lastcall_reset(void);
/* The row that can be played again (the newest P25 call, while the P25 receiver still holds its frames), -1 none */
int ui_lastcall_playable(void);
#if CONFIG_DUAL_WATCH
/* ui_dual.c (plus_xband.py): the watched channel v is tuned to: 'A'..'F' (the ring), 'A' / 'B' (dual watch), 0 none */
char ui_dual_letter_here(const radio_view_t *v);
#endif

#endif
