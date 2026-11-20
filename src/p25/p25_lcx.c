/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DDM32 Plus P25 extras: see p25_lcx.h. */
#include "p25_lcx.h"
#include <string.h>

int p25x_decode_frame(const uint8_t *air, size_t bytes, p25_decoded *d, p25x_lc *x)
{
    x->target_id = 0;
    x->lc_other = 0;
    int r = p25_decode_frame(air, bytes, d);
    if (r || d->metadata_valid || (d->nid.duid != P25_DUID_LDU1 && d->nid.duid != P25_DUID_TDULC)) return r;
    /* The binding left the LC's first three octets in place when its Reed-Solomon code held and the format was not
     * group voice; when the RS code failed they are all zero (format 0, MFID 0): damage, as before */
    uint8_t format = d->link_control_format, mfid = d->manufacturer_id;
    if ((format != 0 && format != 3) || mfid > 1) {
        x->lc_other = !(format & 0x80);
        return r;
    }
    if (format != 3) return r;
    /* Unit to unit: the library decodes it, the binding refuses it (format != 0). Decode the frame once more for the
     * LC octets, only for such an LC (a private call's LDU1s and TDULC), with the binding's own sync tolerance */
    p25cai_frame frame;
    p25cai_quality q;
    p25cai_link_control lc;
    if (p25cai_frame_decode_ex(air, bytes, 4, &frame, &q) < 0 || !q.metadata_valid ||
        p25cai_lc_decode(frame.metadata, &lc) < 0 || lc.format != 3) return r;
    d->metadata_valid = 1;
    d->link_control_format = 3;
    d->manufacturer_id = lc.manufacturer;
    d->service_options = lc.service;
    d->talkgroup = 0;
    d->source_id = lc.source;
    d->clear_voice = !(lc.service & 0x40);
    x->target_id = lc.target;
    return r;
}
