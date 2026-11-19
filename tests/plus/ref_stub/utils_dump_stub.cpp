// SPDX-License-Identifier: GPL-3.0-or-later
// Host-test stand-in for the reference CUtils::dump the reference P25 coder calls when its RS decoder throws
// (tests/plus/test_p25lc_ref.cpp); the reference Utils.cpp needs the full logger. It prints nothing.
#include "Utils.h"

void CUtils::dump(int level, const std::string& title, const unsigned char* data, unsigned int length)
{
	(void)level;
	(void)title;
	(void)data;
	(void)length;
}
