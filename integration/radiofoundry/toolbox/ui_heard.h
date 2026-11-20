/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM25 Plus Tools > Heard list, the UI task's side (ui_heard.c, heard.h). Installed as core/ui/ui_heard.h. */
#ifndef PLUS_UI_HEARD_H
#define PLUS_UI_HEARD_H

#include <stdbool.h>
#include "ui/ui_nav.h"
#include "ui/heard.h"

/* ui_app.c's tick, before it feeds Recent: the view the calls below were heard on (frequency, DMR colour code, slot) */
void ui_heard_view(const radio_view_t *v);
/* A Recent entry: new_call when recent_add takes it in, false while that call goes on (a DMR or P25 call heard again) */
void ui_heard_recent(const ui_recent_t *r, bool new_call);
const heard_list_t *ui_heard_list(void);
void ui_heard_reset(void);

#endif
