// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus: every mode in the VFO (CONFIG_VFO_MODES, docs/plus.md "Every mode in the VFO"). The VFO (a channel_t in
// the radio core, vfo_ch) may be FM, DMR, P25, YSF, NXDN, dPMR or AM, each with the parameters a channel of that mode
// has (P25: NAC, talkgroup, unit ID; YSF: DG-ID; NXDN: RAN; dPMR: the colour code filter; DMR: color code, timeslot and
// RX group list with "Any"). These helpers are pure (no RTOS, no board but radio_am_capable): the radio core, the UI
// and the host tests share them.
//
// AM is not a channel mode: it is FM received through the AM demodulator, channel_t.am (set only in the VFO). Its
// "mode" here is VFOX_AM, the value the settings record keeps for it.
//
// The settings record (settings_t) keeps upstream's layout and size: the VFO's mode stays in its mode field (a value
// upstream doesn't know reads there as FM), and a native mode's parameters go into the VFO's FM tone fields, which that
// mode has no use for. docs/plus.md has the byte map; vfox_store / vfox_load are the only code that knows it.
#ifndef VFOX_H
#define VFOX_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"
#include "settings/settings.h"

#define VFOX_AM		6u	// the VFO's AM, after the codeplug's modes (FM 0, DMR 1, P25 2, YSF 3, NXDN 4, dPMR 5)
#define VFOX_MODES	7u

// The VFO's settings-record ext word (VFO 1: settings_t.unused_dmr_id; VFO 2: vfo2_talk bits 24-26), DMR only
#define VFOX_EXT_MAGIC		0x50000000u	// VFO 1's word: the Plus tag, shared with the priority channel (scanx)
#define VFOX_EXT_SHIFT		11u		// VFO 1's flags in bits 11-13 (scanx uses 0-10 and 16-22)
#define VFOX_EXT_BITS		(7u << VFOX_EXT_SHIFT)
#define VFOX_EXT_ANY_CC		0x01u		// color code Any
#define VFOX_EXT_ANY_TS		0x02u		// timeslot Any
#define VFOX_EXT_ANY_TG		0x04u		// RX group list Any (every talkgroup)
#define VFOX_NATIVE_MARK	0x80u		// set in both tone type bytes: they hold a native mode's parameters

// What the VFO is: VFOX_AM for AM, else the channel mode
uint8_t vfox_mode_of(const channel_t *c);
// This build has mode m in the VFO
bool vfox_mode_built(uint8_t m);
// A native-modem or software-demodulator mode (P25, YSF, NXDN, dPMR): parameters in place of tones
bool vfox_native(uint8_t m);
// The mode after c's in the cycle FM > DMR > P25 > YSF > NXDN > dPMR > AM > FM, only the modes built
uint8_t vfox_mode_next(const channel_t *c);
// "FM", "DMR", "P25", "YSF", "NXDN", "dPMR", "AM"
const char *vfox_mode_name(uint8_t m);
// c into mode m with that mode's defaults (P25: NAC Any, talkgroup Any, the radio's DMR ID as its unit ID when it
// fits; YSF / NXDN / dPMR: Any), the other modes' fields cleared. Frequencies, tones and power stay
void vfox_set_mode(channel_t *c, uint8_t m, uint32_t own_id);
// What follows from the mode and its parameters: values clamped, 12.5 kHz for the digital modes, RX only for YSF /
// NXDN / dPMR and for P25 with NAC Any, talkgroup Any or no unit ID (the TX rule of a P25 channel, codeplug_p25_valid)
void vfox_fix(channel_t *c);
// A channel copied into the VFO (Copy to VFO): its mode's parameters as the channel has them (vfo: already the
// channel's frequencies, mode, tones and DMR color code / slot, as radio_sm.c vfo_copy set them)
void vfox_copy(channel_t *vfo, const channel_t *ch);

// The settings record. vfox_store after upstream's persist_vfo wrote VFO idx (0 or 1) from v: the mode as the VFO's
// own, a native mode's parameters, DMR's Any flags. vfox_load after upstream's restore_vfo read it into v: the same
// back (an older record, or one that upstream firmware wrote, reads as it did before)
void vfox_store(settings_t *st, uint8_t idx, const channel_t *v);
void vfox_load(const settings_t *st, uint8_t idx, channel_t *v);

// Multi-watch (CONFIG_MULTI_WATCH): every ring letter may be a VFO of its own. A and B keep upstream's two VFOs (saved
// in the settings); C..F each have one more, RAM only, kept in this compact form while not in use (32 bytes: what a
// VFO holds, the DMR talk target included). The radio core parks VFO A / B the same way while a C..F one is in use
typedef struct {
	uint32_t rx_hz, tx_hz;
	uint32_t id;		// P25 unit ID (native modes) / DMR own ID
	uint32_t talk;		// the DMR talk target: ID in bits 0-23, bit 31 a private call
	uint16_t tg, nac;	// P25 talkgroup (DG-ID, RAN, dPMR CC), NAC (native modes) / contact, RX group list
	uint16_t rx_tone, tx_tone;
	uint8_t rx_type, tx_type;
	uint8_t mode;		// vfox_mode_of in bits 0-3; bit 7: set (seeded) at all
	uint8_t bw, power, cc, ts, spare;	// CONFIG_P25_TXNAC: the P25 TX NAC, bits 0-7 in spare, 8-12 in bw's bits 3-7
} vfox_slot_t;

void vfox_slot_put(vfox_slot_t *s, const channel_t *c, uint32_t talk, bool seeded);
// A slot never set reads as a fresh VFO (FM, 12.5 kHz, the default power, color code 1, slot 1): seeded false
void vfox_slot_get(const vfox_slot_t *s, channel_t *c, uint32_t *talk, bool *seeded);

#endif
