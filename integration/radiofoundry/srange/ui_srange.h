/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus search ranges (docs/plus.md "Search ranges and speed", CONFIG_SEARCH_RANGE): the range and step of
 * Tools > P25 search and DMR search, and their range editor. Installed as core/ui/ui_srange.{c,h}; UI task only.
 *
 * Each search screen keeps its own ui_srange_t (static: the last range and step stay while the radio is on). A range
 * is a preset (around the frequency in use, or a band) or the manual one typed in the editor; the step is one of
 * srange.h's ten (2.5 kHz to 1 MHz). Left / Right on the screen go through the presets (the manual one too, once it
 * is set); * or a digit opens the editor, which takes the screen's whole page until OK applies it or Back leaves:
 *
 *   Lower  430 MHz        digits type it (positional, as the VFO entry), OK: on to Upper
 *   Upper  440 MHz        Left / Right -/+ one step, held -/+ 1 MHz (Lower, Upper, Move)
 *   Step   12.5 kHz       Left / Right: the other steps
 *   Move   10 MHz wide    the whole range -/+ one step, held -/+ 1 MHz
 *
 * A range must lie in one of the radio's transmit bands (136-174, 400-480 MHz: the receiver the searches step) with
 * its end above its start. */
#ifndef PLUS_UI_SRANGE_H
#define PLUS_UI_SRANGE_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_input.h"
#include "font.h"

enum { SRANGE_P_500K, SRANGE_P_1M, SRANGE_P_2M5, SRANGE_P_UHF, SRANGE_P_VHF, SRANGE_P_MANUAL, SRANGE_P_COUNT };

typedef struct {
	uint8_t preset;		/* SRANGE_P_* */
	uint8_t step;		/* the step's tag (srange.h) */
	uint32_t lo_hz, hi_hz;	/* the manual range, 0 = none set yet */
} ui_srange_t;

#define UI_SRANGE_INIT	{ SRANGE_P_1M, 0u, 0u, 0u }

/* The range to sweep now (presets around center_hz) */
void ui_srange_bounds(const ui_srange_t *r, uint32_t center_hz, uint32_t *lo, uint32_t *hi);
/* The search request's start: the range's start with the step's tag; *hi its end */
uint32_t ui_srange_request(const ui_srange_t *r, uint32_t center_hz, uint32_t *hi);
/* Left / Right on the screen: the next or previous preset (the manual one only once it is set) */
void ui_srange_turn(ui_srange_t *r, int dir);
/* The screen's range line in font f, at most max_w wide: "Range +-1 MHz  12.5k" (shorter forms when it is wider) */
void ui_srange_line(const ui_srange_t *r, const font_t *f, int max_w, char *out, unsigned len);

/* The editor on r (one at a time), started from the range swept now; digit: the first digit typed into Lower, or 0 */
void ui_srange_open(ui_srange_t *r, uint32_t center_hz, char digit);
void ui_srange_close(void);
bool ui_srange_editing(void);
/* A key while the editor is open: UI_SRANGE_KEPT (taken), UI_SRANGE_APPLIED (r holds the new range: a running search
 * starts again on it; the editor closed), UI_SRANGE_LEFT (Back: closed, r unchanged) */
enum { UI_SRANGE_NONE, UI_SRANGE_KEPT, UI_SRANGE_APPLIED, UI_SRANGE_LEFT };
int ui_srange_key(const input_event_t *ev);
/* The editor's rows from y down, and its hint */
void ui_srange_draw(int y);
const char *ui_srange_hint(void);

#endif
