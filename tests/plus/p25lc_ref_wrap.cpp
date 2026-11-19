// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus host test glue (tests/plus/test_p25lc_ref.cpp): MMDVMHost's CP25Data LDU1 link control coder behind two
// plain functions, in a file of its own (its P25Defines.h and our p25.h name the same DUIDs).
#include "P25Data.h"
#include "P25Defines.h"
#include "p25lc_ref_wrap.h"

bool ref_decode_ldu1(const unsigned char *frame, ref_lc *out)
{
	CP25Data d;
	if (!d.decodeLDU1(frame))
		return false;
	out->lcf = d.getLCF();
	out->mfid = d.getMFId();
	out->emergency = d.getEmergency();
	out->dst = d.getDstId();
	out->src = d.getSrcId();
	return true;
}

void ref_encode_ldu1(unsigned char *frame, const ref_lc *in)
{
	CP25Data d;
	d.setLCF(in->lcf);
	d.setMFId(in->mfid);
	d.setEmergency(in->emergency);
	d.setDstId(in->dst);
	d.setSrcId(in->src);
	d.encodeLDU1(frame);
}

unsigned char ref_lcf_group() { return P25_LCF_GROUP; }
unsigned char ref_lcf_private() { return P25_LCF_PRIVATE; }
