// SPDX-License-Identifier: GPL-3.0-or-later
// Host-test stand-ins for the two reference CUtils helpers the reference Golay and CRC code calls
// (tests/plus/test_ysf_ref.cpp); the reference Utils.cpp needs the full logger.
#include "Utils.h"

unsigned int CUtils::countBits(unsigned int v)
{
	unsigned int n = 0U;
	while (v != 0U) {
		v &= v - 1U;
		n++;
	}
	return n;
}

void CUtils::bitsToByteBE(const bool* bits, unsigned char& byte)
{
	byte = 0U;
	for (unsigned int i = 0U; i < 8U; i++)
		byte = (unsigned char)((byte << 1) | (bits[i] ? 1U : 0U));
}
