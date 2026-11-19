/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus YSF transmit: Voice FR ("VW", FICH DT 3) framing for the native 4FSK modem (docs/ysf-nxdn-tx.md).
 *
 * A transmission is a sequence of 960-bit frames (100 ms each at 4800 symbols/s), built one at a time in the
 * caller's frame buffer and handed out bit by bit (ysf_tx_take) to the 288-bit native modem blocks:
 *
 *   header       FI 0, FN 0, FT 1; CSD1 = destination "**********" + our callsign, CSD2 = blank down/uplink
 *   sub-header   FI 1, FN 0, FT 1; CSD3 (rem1..rem4) in payload bytes 0..44, IMBE voice in sections 3 and 4
 *   voice        FI 1, FN 0, FT 0; IMBE voice in all five sections (100 ms of speech)
 *   ...
 *   terminator   FI 2, FN 0, FT 1; CSD1 and CSD2 as in the header
 *
 * Every FICH: CS 2, CM 0 (group), BN 0, BT 0, MR 0 (direct), VoIP 0, Dev YSF_TX_LOW_DEV, DT 3, SQL 0, SQ the
 * DG-ID. Why this form: MMDVMHost (G4KLX, GPL-2.0-or-later), built against real Yaesu radios, treats FN 0 / FT 1
 * as "the first packet after the header" (CSD3 data channel + 2 IMBE frames, CYSFControl::processVWData) and forces
 * FN 0 / FT 0 for a Voice FR frame whose FICH it can't read; DSDcc takes the frame after a header as the CSD3
 * sub-header and every later one as 5 IMBE frames. Our receiver (ysf_vw.h) follows MMDVMHost. The IMBE sections
 * are P25's channel code (ysf_vw.h), u7 bit 0 alternating frame by frame as in a P25 LDU.
 *
 * Use (the P25 worker, p25_lab.c): ysf_tx_begin, then while frames go out: ysf_tx_take into the modem blocks;
 * whenever ysf_tx_need_voice, one 20 ms IMBE frame (ysf_tx_voice); on PTT release, once the microphone backlog is
 * encoded, ysf_tx_close (pad with encoded silence while ysf_tx_need_voice stays true), until ysf_tx_done. */
#ifndef YSF_TX_H
#define YSF_TX_H

#include <stdbool.h>
#include <stdint.h>
#include "ysf.h"

#ifndef YSF_TX_LOW_DEV
#define YSF_TX_LOW_DEV	0	/* FICH "Dev" bit: 0 = normal deviation (MMDVMHost's default), 1 = half */
#endif

enum { YSF_TX_HEADER, YSF_TX_SUBHEADER, YSF_TX_VOICE, YSF_TX_TERMINATOR, YSF_TX_DONE };

typedef struct {
	uint8_t *frame;		/* the frame in hand (YSF_FRAME_BYTES; the caller's buffer) */
	uint8_t call[10];	/* our callsign, space padded */
	uint8_t dgid;
	uint8_t phase;		/* the frame in hand: YSF_TX_* */
	uint8_t voice_in;	/* IMBE frames written into it so far */
	uint8_t closing;	/* ysf_tx_close was called */
	uint16_t sent;		/* its bits handed out so far */
	uint32_t frames;	/* frames handed out completely */
	uint32_t imbe;		/* IMBE frames written */
} ysf_tx_t;

/* Start a transmission: callsign (up to 10 characters, NUL ended), DG-ID 0..127; the header is ready at once */
void ysf_tx_begin(ysf_tx_t *t, uint8_t frame[YSF_FRAME_BYTES], const char *call, uint8_t dgid);
/* The frame in hand waits for IMBE voice (it can't be handed out yet) */
bool ysf_tx_need_voice(const ysf_tx_t *t);
/* One IMBE frame (u0..u7 as dm32_imbe_encode gives them); ignored unless ysf_tx_need_voice */
void ysf_tx_voice(ysf_tx_t *t, const uint16_t u[8]);
/* PTT released and the microphone backlog encoded: a voice frame with no IMBE yet becomes the terminator; one that
 * is part filled still needs voice (pad it with encoded silence), then the terminator follows */
void ysf_tx_close(ysf_tx_t *t);
/* Up to n bits of the frame in hand into out[] from bit at on (MSB first), as far as it is ready; returns the bits
 * written. A frame handed out completely makes way for the next one */
unsigned ysf_tx_take(ysf_tx_t *t, uint8_t *out, unsigned at, unsigned n);
/* The terminator has been handed out */
bool ysf_tx_done(const ysf_tx_t *t);

/* The FICH information bytes this transmitter writes (exposed for the tests) */
void ysf_tx_fich_raw(uint8_t fi, uint8_t fn, uint8_t ft, uint8_t dgid, uint8_t raw[4]);

#endif
