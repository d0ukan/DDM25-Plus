# SPDX-License-Identifier: GPL-3.0-or-later
"""DDM32 Plus: the status strip's signal bars show a signal only while one is received (owner's report after
v1.0.0-beta.4). Called from plus_features.prepare(); one line there.

Upstream draws the bars from the receiver's raw level at every poll. With nothing on the channel the noise floor
wanders between about -118 and -98 dBm, so the bars climbed and fell between one and three on any channel and in any
mode, as if a call or data were coming in. Now the bars are filled only while something is heard:

  - FM / AM / P25: the squelch is open (RS_RX: carrier and tone; P25 holds RS_RX while a call is heard);
  - DMR: a call is heard on our slot (RDMR_RX, RDMR_RX_OTHER), at the call's own level when the modem gives one.

Otherwise the bars are drawn empty. Home's dBm meter and the Tools screens still show the raw level (a meter's job).
"""

OLD_CALL = ('\t\tgfx_icon_bars(rx, ty + f->ascent - bh - 1, sw, bh + 1, 4, rssi_bars(v->rssi_dbm), sfg, '
            'filled ? ui_col.idle : ui_col.line);\n')
NEW_CALL = ('\t\tgfx_icon_bars(rx, ty + f->ascent - bh - 1, sw, bh + 1, 4, strip_bars(v), sfg, '
            'filled ? ui_col.idle : ui_col.line);\n')

OLD_FN = '''	if (dbm >= -118) return 1;
	return 0;
}
'''
NEW_FN = OLD_FN + '''
// DM32 Plus: the strip's bars while something is heard, empty otherwise (tools/plus_sigbar.py)
static uint8_t strip_bars(const radio_view_t *v)
{
#if CONFIG_DMR
	if (v->dmr.state == RDMR_RX || v->dmr.state == RDMR_RX_OTHER)
		return rssi_bars(v->dmr.rssi_dbm != 0 ? v->dmr.rssi_dbm : v->rssi_dbm);
#endif
	return v->state == RS_RX ? rssi_bars(v->rssi_dbm) : 0;
}
'''


def prepare_sigbar(transform, install):
    transform('core/ui/ui_screens.c', [(OLD_FN, NEW_FN), (OLD_CALL, NEW_CALL)])
