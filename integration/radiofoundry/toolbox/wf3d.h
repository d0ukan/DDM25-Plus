/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus 3D waterfall (CONFIG_SPECTRUM_WF3D, item 45, docs/plus.md "Round 14: air band and spectrum"): the
 * waterfall's rows (wfall.h) drawn as a ridge plot in perspective, Tools > Spectrum's third view. Installed as
 * core/ui/wf3d.{c,h}. No RAM of its own and no buffer: every point is worked out from the rows as it is drawn, so the
 * UI task's stack holds a few integers only.
 *
 * The picture: the newest WF3D_ROWS rows, the newest in front at the bottom of the box and full width, each older one
 * further back: x drawn towards the centre and the row raised by its depth (scale WF3D_K / (WF3D_K + depth), a
 * vanishing point above the box chosen so the last row's highest peak just reaches the top). A point's level lifts
 * its row by up to WF3D_AMP of the box's height at the front. Back to front (painter's order): each row first clears
 * the area under its ridge down to its own base (the rows behind are hidden there), then draws the ridge in the colour
 * of its level's slot (wf_slot; slot 0, the background's, is drawn in slot 1's so a quiet row still shows). */
#ifndef PLUS_WF3D_H
#define PLUS_WF3D_H

#include <stdint.h>
#include "ui/gfx.h"
#include "ui/wfall.h"

#define WF3D_ROWS	24u	/* rows drawn, the newest */
#define WF3D_K		16	/* the depth scale: row z is drawn at WF3D_K / (WF3D_K + z) of the front's size */
#define WF3D_AMP_NUM	9	/* level 15 lifts the front row by 9 / 20 of the box's height */
#define WF3D_AMP_DEN	20

/* w's rows in the box (x0 .. x0 + points * bw - 1, top .. top + h - 1): the front row's point i at x0 + i * bw + bw / 2.
 * slot[]: the colours of wf_slot's slots; bg: the background (the area under each ridge). Nothing outside the box */
void wf3d_draw(const wf_t *w, int x0, int bw, int top, int h, const color_t slot[WF_SLOTS], color_t bg);

#endif
