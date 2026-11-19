/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus NXDN48 receive: the DM-32's flat FM receive audio for the software demodulator (docs/nxdn.md,
 * core/radio/radio_nxdn.h board_nxdn_flat). Installed as boards/dm32/dm32_nxdn.inc and included at the end of
 * boards/dm32/radio_dm32.c (after dm32_native.inc) when CONFIG_NXDN=y: it uses that file's statics fd, modem, mode,
 * transmitting and have_rx.
 *
 * The audio itself comes through the APRS receive path, unchanged: hrc_afsk rx_start() -> aprs_listen_route(true)
 * keeps the FD6818's AF output on the HR_C7000's input (PTB15 = 0), and fm_rx_audio.c copies the HR_C7000's 8 kHz FM
 * RX RAM halves (SYS_INTERP b14) into a 1024-sample heap ring. With the channel's mode not RADIO_MODE_FM, radio_sm's
 * outputs() keeps the squelch "closed" (rx_audio false), so speaker_source() neither plays the FD6818 to the speaker
 * nor the HR_C7000's FM playback: the 4FSK is not heard, only the decoded voice the vocoder plays on the PCM path.
 *
 * What this file adds is the register recipe that makes that audio the discriminator's rather than voice audio, from
 * DM32-nxdn-p25 integration/radiofoundry/nxdn/dm32_nxdn.inc (GPL-3.0-or-later, "lyra XE1POC"; field-reported to
 * receive Icom and Kenwood NXDN48 on a DM-32, their issue #4):
 *   - FD6818 reg 0x2B (AF filters) = 0x0700: receive 300 Hz high-pass, 3 kHz low-pass and de-emphasis bypassed (the
 *     stock firmware's digital word, our fd6818.c DMR table writes it too); FM's table writes 0xC000.
 *   - FD6818 reg 0x48 (AF gain) low nibble 15. Our FM and DMR tables already write 0xBBBF (nibble 15): kept for a
 *     table that changes, a no-op today.
 *   - HR_C7000 FM_BANDWIDTH (0x500) b6 cleared: its FM band-pass off. hrc7000_fm_rx(), which every rx() runs in FM,
 *     sets it again: board_nxdn_flat(true) is idempotent and radio_nxdn.c calls it after each retune and once a second.
 * Leaving restores 0x2B = 0xC000, the saved gain and b6.
 *
 * HARDWARE-UNVERIFIED on our tree. To check on the radio (docs/nxdn.md): the RX RAM sample sign (the frame layer
 * hunts both polarities), the level of a 1.05 kHz-deviation tone in RX RAM (should sit well below full scale), the
 * flat response (no de-emphasis droop at 1.2 kHz), and that nothing of the 4FSK reaches the speaker. */
#if CONFIG_NXDN
#define NXDN_FD_AF_FILTER	0x2Bu
#define NXDN_FD_AF_FILTER_FM	0xC000u		/* fd6818.c's FM table */
#define NXDN_FD_AF_FILTER_FLAT	0x0700u		/* receive HPF, LPF and de-emphasis bypassed (stock digital word) */
#define NXDN_FD_AF_GAIN		FD6818_REG_VOLUME	/* 0x48: bits 3:0 AF DAC gain */
#define NXDN_FD_AF_GAIN_FLAT	15u
#define NXDN_C7_FM_BW_BANDPASS	(1u << 6)

static bool nxdn_flat_on, nxdn_bandpass_was;
static uint16_t nxdn_gain_saved;

bool board_nxdn_flat(bool on)
{
	if (on) {
		if (mode != RADIO_MODE_FM || transmitting || !have_rx) {
			return false;
		}
		if (!nxdn_flat_on) {
			nxdn_gain_saved = fd6818_read(&fd, NXDN_FD_AF_GAIN);
			fd6818_write(&fd, NXDN_FD_AF_FILTER, NXDN_FD_AF_FILTER_FLAT);
			fd6818_write(&fd, NXDN_FD_AF_GAIN, (uint16_t)((nxdn_gain_saved & 0xFFF0u) | NXDN_FD_AF_GAIN_FLAT));
			nxdn_bandpass_was = (hrc7000_rd(&modem, C7_FM_BANDWIDTH) & NXDN_C7_FM_BW_BANDPASS) != 0;
			nxdn_flat_on = true;
		}
		// Every tune in FM sets the band-pass again (hrc7000_fm_rx): take it off whenever it is back
		uint32_t bw = hrc7000_rd(&modem, C7_FM_BANDWIDTH);
		if ((bw & NXDN_C7_FM_BW_BANDPASS) != 0) {
			hrc7000_wr(&modem, C7_FM_BANDWIDTH, bw & ~NXDN_C7_FM_BW_BANDPASS);
		}
		return true;
	}
	if (nxdn_flat_on) {
		nxdn_flat_on = false;
		fd6818_write(&fd, NXDN_FD_AF_FILTER, NXDN_FD_AF_FILTER_FM);
		fd6818_write(&fd, NXDN_FD_AF_GAIN, nxdn_gain_saved);
		if (nxdn_bandpass_was && mode == RADIO_MODE_FM && !transmitting) {
			hrc7000_wr(&modem, C7_FM_BANDWIDTH, hrc7000_rd(&modem, C7_FM_BANDWIDTH) | NXDN_C7_FM_BW_BANDPASS);
		}
	}
	return true;
}
#endif
