/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DM32 Plus YSF Voice FR mode voice channel (ysf_vw.h). */
#include "ysf_vw.h"

static inline unsigned get_bit(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7u - (i & 7u))) & 1u; }
static inline void put_bit(uint8_t *p, unsigned i, unsigned b)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7u));
	p[i >> 3] = b ? (uint8_t)(p[i >> 3] | m) : (uint8_t)(p[i >> 3] & ~m);
}

/* Codeword bit j sits at section bit IL(j): six rows of 24, the bit pair swapped in odd columns (IMBE_INTERLEAVE) */
static unsigned il(unsigned j) { return 6u * (j % 24u) + ((j / 24u) ^ (j & 1u)); }

/* First bit of section s: past the sync and FICH (240 bits), 144 bits a section */
static unsigned section_base(unsigned s) { return 240u + 144u * s; }

unsigned ysf_vw_voice_count(const ysf_fich_t *f)
{
	if (f->fi != YSF_FI_COMMS || f->dt != YSF_DT_VOICE_FR)
		return 0;
	return (f->fn == 0u && f->ft == 1u) ? 2u : YSF_VW_SECTIONS;
}

unsigned ysf_vw_voice_section(const ysf_fich_t *f, unsigned k)
{
	return ysf_vw_voice_count(f) == 2u ? 3u + k : k;
}

void ysf_vw_section(const uint8_t frame[YSF_FRAME_BYTES], unsigned s, uint8_t cw[YSF_VW_IMBE_BYTES])
{
	unsigned base = section_base(s);
	for (unsigned j = 0; j < YSF_VW_IMBE_BITS; j++)
		put_bit(cw, j, get_bit(frame, base + il(j)));
}

void ysf_vw_section_put(const uint8_t cw[YSF_VW_IMBE_BYTES], unsigned s, uint8_t frame[YSF_FRAME_BYTES])
{
	unsigned base = section_base(s);
	for (unsigned j = 0; j < YSF_VW_IMBE_BITS; j++)
		put_bit(frame, base + il(j), get_bit(cw, j));
}

bool ysf_vw_voice(const uint8_t frame[YSF_FRAME_BYTES], const ysf_fich_t *f, unsigned k,
		  uint8_t cw[YSF_VW_IMBE_BYTES])
{
	if (k >= ysf_vw_voice_count(f))
		return false;
	ysf_vw_section(frame, ysf_vw_voice_section(f, k), cw);
	return true;
}

bool ysf_vw_voice_put(const uint8_t cw[YSF_VW_IMBE_BYTES], const ysf_fich_t *f, unsigned k,
		      uint8_t frame[YSF_FRAME_BYTES])
{
	if (k >= ysf_vw_voice_count(f))
		return false;
	ysf_vw_section_put(cw, ysf_vw_voice_section(f, k), frame);
	return true;
}
