/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus YSF Voice FR mode ("VW", FICH DT = 3): the full-rate IMBE voice channel (docs/plus.md).
 *
 * VW carries the P25 Phase 1 full-rate IMBE codec under exactly the P25 voice FEC, so the YSF layer only has to
 * move bits: each voice section is the 144-bit channel-coded IMBE frame that src/p25/p25.c p25_imbe_decode()
 * takes (and p25_imbe_encode() produces), after a 6 x 24 bit de-interleave. The layout constants follow the
 * GPL-2.0-or-later MMDVMHost (G4KLX): CYSFPayload::processVoiceFRModeAudio2/Audio5 and CYSFControl's RF/network
 * VW paths, CAMBEFEC::regenerateIMBE and its IMBE_INTERLEAVE table; and MMDVM_CM YSF2P25 (EA7EE)
 * CModeConv::putYSF/decode. The host tests check this code against those programs bit for bit
 * (tools/test_ysf_vw_ref.sh).
 *
 * Frame layout (air order, MSB first, as ysf.h): frame[0..4] sync, frame[5..29] FICH, frame[30..119] the 720-bit
 * payload = five 144-bit sections, section s at payload byte 18 * s (frame byte 30 + 18 * s), s = 0..4.
 *
 * Which sections are voice (the FICH must be FI = communications, DT = Voice FR; header and terminator frames carry
 * no voice):
 *  - FN = 0 and FT = 1, the first frame after the header (MMDVMHost: "The first packet after the header is odd"):
 *    payload bytes 0..44 are the VW data channel (180 dibits, K=5 rate 1/2 convolutional code, 9 x 20 dibit
 *    interleave, CRC-CCITT; processVoiceFRModeData, not decoded here), bytes 45..53 are left alone by the
 *    reference, and only sections 3 and 4 are voice (processVoiceFRModeAudio2): 2 IMBE frames.
 *  - every other communications frame (any other FN/FT pair, FN = 0 included): all five sections are voice
 *    (processVoiceFRModeAudio5): 5 IMBE frames, 100 ms of audio.
 * MMDVM_CM YSF2P25 instead takes all five sections of every communications frame; we follow MMDVMHost.
 *
 * One section's 144 bits: codeword bit j (0..143, cw[] MSB first, the P25 order c0 Golay(23,12) bits 0..22,
 * c1..c3 PN-whitened Golay(23,12) at 23, 46, 69, c4..c6 PN-whitened Hamming(15,11) at 92, 107, 122, c7 seven
 * plain bits at 137) is section bit 6 * (j % 24) + ((j / 24) ^ (j & 1)) (IMBE_INTERLEAVE; the same permutation as
 * the P25 LDU voice_permutation() in p25.c). Receive path: ysf_vw_voice() -> p25_imbe_decode(cw, u, &e0) ->
 * dm32_imbe_decode(ctx, u, pcm). This layer has no FEC of its own: correction (and the error counts for gating
 * audio) come from p25_imbe_decode. */
#ifndef YSF_VW_H
#define YSF_VW_H

#include <stdbool.h>
#include <stdint.h>
#include "ysf.h"

#define YSF_VW_SECTIONS		5u	/* 144-bit sections in a frame's payload */
#define YSF_VW_IMBE_BITS	144u
#define YSF_VW_IMBE_BYTES	18u

/* How many IMBE frames this frame carries: 0 (not a VW communications frame), 2 (FN 0, FT 1) or 5 */
unsigned ysf_vw_voice_count(const ysf_fich_t *fich);
/* The section (0..4) holding voice frame k (0..count-1) */
unsigned ysf_vw_voice_section(const ysf_fich_t *fich, unsigned k);

/* Voice frame k as the de-interleaved 144-bit coded IMBE frame (p25_imbe_decode's cw[18]); false (cw untouched)
 * when k >= ysf_vw_voice_count(fich) */
bool ysf_vw_voice(const uint8_t frame[YSF_FRAME_BYTES], const ysf_fich_t *fich, unsigned k,
		  uint8_t cw[YSF_VW_IMBE_BYTES]);
/* Write voice frame k from a coded IMBE frame (p25_imbe_encode's cw[18]); tests, TX later */
bool ysf_vw_voice_put(const uint8_t cw[YSF_VW_IMBE_BYTES], const ysf_fich_t *fich, unsigned k,
		      uint8_t frame[YSF_FRAME_BYTES]);

/* Raw section access (s = 0..4), whatever the FICH says */
void ysf_vw_section(const uint8_t frame[YSF_FRAME_BYTES], unsigned s, uint8_t cw[YSF_VW_IMBE_BYTES]);
void ysf_vw_section_put(const uint8_t cw[YSF_VW_IMBE_BYTES], unsigned s, uint8_t frame[YSF_FRAME_BYTES]);

#endif
