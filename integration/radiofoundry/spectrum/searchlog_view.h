/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus search log: the debug link sections "p25s" and "dmrs" (searchlog_view.c, tools/plus_searchlog.py) */
#ifndef PLUS_SEARCHLOG_VIEW_H
#define PLUS_SEARCHLOG_VIEW_H

#include "debuglink/dl_fmt.h"

void searchlog_view_p25(dl_text_t *t);
void searchlog_view_dmr(dl_text_t *t);

#endif
