/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus tone finder: the FD6818's sub-audio measurement registers, read only (core/radio/radio_fmdec.h
 * board_css_read). Installed as boards/dm32/dm32_css.inc and included at the end of boards/dm32/radio_dm32.c when
 * CONFIG_TONE_FIND=y: it uses that file's statics fd, mode, transmitting and have_rx.
 *
 * What is known: the FD6818 detects one CTCSS tone or DCS code it is set up for (0x51, 0x07, 0x08; the verdict in
 * 0x0C, hardware-dm32.md 8.4), which is how the radio's squelch works; stepping it through 50 tones and 208 codes at a
 * few hundred ms each would take a minute and close the squelch meanwhile, so the tone finder decodes the audio
 * instead (tonefind.h). The DM-1801 Pro's stock firmware also reads 0x68-0x6A (hardware-dm1801pro.md 5.4). On the
 * BK4819, which the FD6818 resembles, those hold the chip's own CTCSS frequency measurement (0x68: bit 15 clear = a
 * tone, bits 12:0 = Hz x 20.64888, the 0x07 scale) and the 23-bit DCS word it receives (0x69 bit 15 clear = a code,
 * bits 11:0 the high half; 0x6A bits 11:0 the low half). HARDWARE-UNVERIFIED on the FD6818: the screen shows what
 * they read beside the software's answer (docs/plus-radio-test.md TF rows), nothing acts on them. Reads only: no
 * register is written. */
#if CONFIG_TONE_FIND
#include "radio/radio_fmdec.h"

bool board_css_read(uint16_t r[3])
{
	if (mode != RADIO_MODE_FM || transmitting || !have_rx)
		return false;
	r[0] = fd6818_read(&fd, 0x68);
	r[1] = fd6818_read(&fd, 0x69);
	r[2] = fd6818_read(&fd, 0x6A);
	return true;
}
#endif
