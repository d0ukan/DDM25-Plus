// SPDX-License-Identifier: GPL-3.0-or-later
// DM32 Plus host test glue: MMDVMHost's LDU1 link control coder (tests/plus/p25lc_ref_wrap.cpp)
#ifndef P25LC_REF_WRAP_H
#define P25LC_REF_WRAP_H
struct ref_lc {
	unsigned char lcf, mfid;
	bool emergency;
	unsigned int dst, src;
};
bool ref_decode_ldu1(const unsigned char *frame, ref_lc *out);	// CP25Data::decodeLDU1 and its getters
void ref_encode_ldu1(unsigned char *frame, const ref_lc *in);	// CP25Data::encodeLDU1 over a whole LDU1
unsigned char ref_lcf_group();
unsigned char ref_lcf_private();
#endif
