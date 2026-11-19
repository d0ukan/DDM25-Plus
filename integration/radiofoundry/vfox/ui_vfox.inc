// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus: every mode in the VFO, the UI's part (CONFIG_VFO_MODES, docs/plus.md "Every mode in the VFO"). Included
// by core/ui/ui_menus.c after the channel editor, whose rows, field editors and checks it shares: "VFO settings"
// (SCR_VFOSET) is the channel editor on a copy of the VFO in use (ced_ch), showing only what a VFO has (Mode, RX, TX,
// and the mode's rows: FM bandwidth and tones; DMR color code, timeslot and RX group list None / Any; P25 talkgroup,
// NAC and unit ID; YSF DG-ID; NXDN RAN; dPMR CC; "Save as new channel"). Each change goes to the radio at once
// (ui_svc_vfo_apply), nothing waits for a save. Opened with # in Quick settings or on the VFO row of Zones, in the VFO.
// The Mode tile of Quick settings cycles the same modes (ui_vfox_quick_mode).

#define CED_VFO_NUM	0xFFFFu		// ced_num on the VFO settings (no codeplug channel has it)

// The rows a VFO has (the channel editor's own rules then hide another mode's)
static bool vfox_row(uint8_t r)
{
	switch (r) {
	case CE_MODE: case CE_RX: case CE_TX: case CE_BW: case CE_CC: case CE_TS:
	case CE_P25_TG: case CE_P25_NAC: case CE_P25_ID: case CE_SAVE_NEW:
#if CONFIG_P25_TXNAC
	case CE_P25_TX_NAC:	// DM32 Plus: the VFO's TX NAC (RAM only)
#endif
		return true;
	case CE_LIST:
		return CONFIG_DMR_ANY;		// None / Any (all groups): the VFO has no RX group lists
	case CE_RX_TONE: case CE_TX_TONE:
		return !ced_ch.am;		// no sub-audio in AM
	default:
		return false;
	}
}

// The copy to the radio; what the screen shows is then what the VFO has
static void __attribute__((noinline)) vfox_apply(void)
{
	vfox_fix(&ced_ch);
	if (!ui_svc_vfo_apply(&ced_ch))
		ui_toast(TR("VFO: not in this build"));
	ced_orig = ced_ch;
}

// A copy of the VFO as a new channel: the channel editor's "New from current" on it (named after its frequency; the
// channel's own rows, a TX contact on DMR, are set there, and hold OK saves). It replaces this screen: both share the
// editor's copy
static void vfox_to_channel(void)
{
	uint16_t num = codeplug_free_channel();
	if (num == 0) {
		ui_toast(TR("No free channel"));
		return;
	}
	vfox_from_vfo = true;
	ced_from_current = true;
	ui_pop();
	ui_push(SCR_CHANNEL, num);
}

// The rows that work differently on the VFO. True: done here
static bool vfox_select(uint8_t r, bool hold)
{
	if (hold) {
		ui_toast(VFOX_APPLIED);
		return true;
	}
	switch (r) {
	case CE_MODE: {
		const radio_view_t *v = ui_view();
		if (radio_am_tx_refused(ced_ch.rx_hz) || v->bcast) {
			ui_toast(v->bcast ? TR("FM only in the broadcast band") : TR("AM only in the air band"));
			return true;
		}
		vfox_set_mode(&ced_ch, vfox_mode_next(&ced_ch), v->dmr.own_id);
		break;
	}
	case CE_LIST:
		ced_ch.listen_list = ced_ch.listen_list == CODEPLUG_LIST_ANY ? 0 : CODEPLUG_LIST_ANY;
		break;
	case CE_SAVE_NEW:
		vfox_to_channel();
		return true;
	default:
		return false;
	}
	vfox_apply();
	ui_dirty();
	return true;
}

static void vfoset_enter(void)
{
	const radio_view_t *v = ui_view();
	ced_vfo = true;
	ced_edit = CE_NONE;
	ced_new = false;
#if CONFIG_NAC_DEC
	ced_nac_dec = false;
#endif
	ced_num = v->vfo ? CED_VFO_NUM : 0;	// only in the VFO (else: nothing to show)
	ced_ch = v->ch;
	ced_orig = ced_ch;
}

static const char *vfoset_title(void) { return VFOX_TITLE; }

static const ui_scr_def_t vfoset_screen = {
	.name = "vfo", .flags = SF_LIST | SF_FIELD, .empty = TRN("VFO: type a frequency"),
	.hint = TRN("OK change  Back"),
	.key = chinfo_key, .count = chinfo_count, .item = chinfo_item, .select = chinfo_select, .title = vfoset_title,
	.enter = vfoset_enter,
};

// Quick settings' Mode tile in the VFO: the next mode of the cycle. False: upstream's FM <-> DMR toggle (and its
// toasts in the air band and the broadcast band)
bool ui_vfox_quick_mode(const radio_view_t *v)
{
	if (!v->vfo || radio_am_tx_refused(v->ch.rx_hz) || v->bcast)
		return false;
	uint8_t m = vfox_mode_next(&v->ch);
	if (!ui_svc_mode(m))
		return false;
	static char msg[12];
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	sb_str(&b, "VFO: ");
	sb_str(&b, vfox_mode_name(m));
	ui_toast(msg);
	return true;
}
