/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus YSF (Yaesu System Fusion, C4FM) air layer (docs/plus.md, "More digital modes"; transmit framing in
 * ysf_tx.h, docs/ysf-nxdn-tx.md).
 *
 * A clean C implementation of the air framing: the 40-bit sync, the FICH (a K=5 rate 1/2 convolutional code over
 * four Golay (24,12) words and a CRC-CCITT, dibit-interleaved), and the V/D mode 2 ("DN") voice channel, whose
 * AMBE+2 frames are the DMR vocoder's 49 parameter bits (12 + 12 + 25) under YSF's own FEC (bit triplets,
 * whitening, a 26 x 4 bit interleave). The layout constants follow the GPL-2.0-or-later MMDVMHost and MMDVM_CM
 * (G4KLX, EA7EE); the host tests check this code against those programs' encoders bit for bit.
 *
 * Bytes are air order, MSB first: frame[0..4] the sync, frame[5..29] the FICH, frame[30..119] the payload. */
#ifndef YSF_H
#define YSF_H

#include <stdbool.h>
#include <stdint.h>

#define YSF_FRAME_BYTES		120u
#define YSF_FRAME_BITS		960u
#define YSF_SYNC		0xD471C9634DULL	/* 40 bits */
#define YSF_SYNC_BITS		40u
#define YSF_SYNC_MAX_ERRORS	3u		/* bits that may differ in a sync accepted while hunting */
#define YSF_VCH_PER_FRAME	5u
#define YSF_AMBE_BITS		49u

enum { YSF_FI_HEADER, YSF_FI_COMMS, YSF_FI_TERMINATOR, YSF_FI_TEST };
enum { YSF_DT_VD1, YSF_DT_DATA_FR, YSF_DT_VD2, YSF_DT_VOICE_FR };

typedef struct {
	uint8_t raw[4];		/* the 32 information bits */
	uint8_t fi, cm, bn, bt, fn, ft, dt, mr, dgid;
	bool dev, voip;
} ysf_fich_t;

/* FICH: false when a Golay word is beyond repair or the CRC fails */
bool ysf_fich_decode(const uint8_t frame[YSF_FRAME_BYTES], ysf_fich_t *out);
void ysf_fich_encode(const uint8_t raw[4], uint8_t frame[YSF_FRAME_BYTES]);	/* sync and FICH bytes 0..29 */
void ysf_fich_fields(const uint8_t raw[4], ysf_fich_t *out);

/* V/D mode 2 voice channel j (0..4): the 49 AMBE+2 parameter bits (one per byte, 0/1, DMR order a12 b12 c25)
 * and how many of its 27 bit triplets were outvoted */
unsigned ysf_vd2_voice(const uint8_t frame[YSF_FRAME_BYTES], unsigned j, uint8_t params[YSF_AMBE_BITS]);
/* Write voice channel j from 49 parameter bits (tests; TX later) */
void ysf_vd2_voice_put(const uint8_t params[YSF_AMBE_BITS], unsigned j, uint8_t frame[YSF_FRAME_BYTES]);

/* V/D mode 2 data channel (DCH, five 40-bit pieces): its 10 bytes when the CRC holds. FN 0 carries the destination
 * callsign, FN 1 the source's (10 characters, space padded) */
bool ysf_vd2_data(const uint8_t frame[YSF_FRAME_BYTES], uint8_t out[10]);
void ysf_vd2_data_put(const uint8_t in[10], uint8_t frame[YSF_FRAME_BYTES]);	/* tests; TX later */

/* The 20-byte callsign data channels (CSD): 20 bytes, PN whitened, CRC-CCITT, K=5 rate 1/2 code, 9 x 20 dibit
 * interleave, 45 coded bytes. A header or terminator frame carries CSD1 (destination 0..9, source 10..19) in the
 * first 9 bytes of each 18-byte payload section and CSD2 (downlink 0..9, uplink 10..19) in the last 9; the first
 * communications frame of a Voice FR call (FN 0, FT 1) carries CSD3 (four 5-byte "rem" fields) in payload bytes
 * 0..44 (MMDVMHost CYSFPayload::processHeaderData / writeHeader / processVoiceFRModeData; DSDcc's names). */
enum { YSF_CSD1, YSF_CSD2, YSF_CSD3 };
#define YSF_CSD_BYTES	20u
void ysf_csd_put(const uint8_t in[YSF_CSD_BYTES], unsigned which, uint8_t frame[YSF_FRAME_BYTES]);
/* false (out untouched) when the CRC fails */
bool ysf_csd_get(const uint8_t frame[YSF_FRAME_BYTES], unsigned which, uint8_t out[YSF_CSD_BYTES]);

/* Golay (24,12): the extended code YSF uses (generator 0xC75 plus overall parity); decode corrects up to 3 bit
 * errors and returns -1 beyond that (exposed for the tests) */
uint32_t ysf_golay24_encode(uint16_t data12);
int ysf_golay24_decode(uint32_t word24, uint16_t *data12);
uint16_t ysf_crc_ccitt(const uint8_t *p, unsigned n);	/* YSF FICH's: poly 0x1021, init 0, inverted */

/* Framer: air bits in, whole frames out. Hunts for the sync (up to YSF_SYNC_MAX_ERRORS bits off), then takes
 * every 960 bits as a frame while the sync at each frame start stays within the limit; a lost sync hunts again */
typedef struct {
	uint64_t shift;		/* the last 40 bits */
	uint8_t frame[YSF_FRAME_BYTES];
	uint16_t have;		/* bits of the frame in hand, 0 = hunting */
	uint32_t frames, lost;
} ysf_framer_t;
void ysf_framer_init(ysf_framer_t *f);
/* One bit; true when frame[] holds a complete frame (sync included) */
bool ysf_framer_bit(ysf_framer_t *f, uint8_t bit);

#endif
