/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 extras (docs/plus.md "P25 emergency and unit-to-unit calls"): the link control formats the Classic
 * binding (src/p25/p25.c, a locked copy of the CAI library's caller binding) leaves undecoded. That binding takes only
 * the group voice LC (LCF 0x00); here the unit-to-unit voice LC (LCF 0x03, TIA-102.AABF: octets 3-5 the called unit,
 * 6-8 the source) is decoded too, through the library's own p25cai_lc_decode, and an LC of another format that passed
 * its FEC (a vendor's talker alias, ...) is told apart from damage. No RTOS, no hardware. */
#ifndef P25_LCX_H
#define P25_LCX_H
#include "p25.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    /* LCF 0x03: the called unit (d->talkgroup 0); 0 for group voice and every other frame */
    uint32_t target_id;
    /* An LDU1/TDULC link control that passed its FEC but is not a voice channel user format decoded here (another LCF,
     * or an MFID above 1): d->metadata_valid 0, as before, and lc_other 1 (protected, P bit set: 0). It says nothing
     * about the call's talkgroup, source or encryption */
    uint8_t lc_other;
} p25x_lc;
/* p25_decode_frame, then the extras above for an LDU1 or TDULC. A unit-to-unit LC comes back as a valid one:
 * d->metadata_valid 1, link_control_format 3, source_id, service_options, manufacturer_id, clear_voice as for group
 * voice, talkgroup 0 and the called unit in x->target_id. Same return value as p25_decode_frame */
int p25x_decode_frame(const uint8_t *air, size_t bytes, p25_decoded *d, p25x_lc *x);
#ifdef __cplusplus
}
#endif
#endif
