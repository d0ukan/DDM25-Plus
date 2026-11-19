/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus QSO log on Recent (qso_log.h): * on a row marks it as a QSO or takes the mark away (the row then shows a
 * star before its name, ui_menus.c recent_item). On Recent, * does this instead of opening the quick settings. */
#include "config.h"
#if CONFIG_QSO_LOG
#include "ui_nav.h"
#include "ui_screens.h"
#include "i18n.h"
#include "ui/qso_log.h"
#include "ui/ui_misc.h"

bool ui_qso_recent_key(const input_event_t *ev)
{
	if (ev->action != ACT_QUICK || ev->gesture != GESTURE_PRESS)
		return false;
	uint16_t i = ui_frame()->focus;
	if (i >= ui_recent_count()) {
		ui_toast(TR("No calls heard yet"));
		return true;
	}
	ui_toast(qso_log_toggle((uint8_t)i) ? TR("Marked as QSO") : TR("QSO mark removed"));
	ui_dirty();
	return true;
}
#endif
