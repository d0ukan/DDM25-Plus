#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# DM32 Plus: check the P25 extras' link control (src/p25 with CONFIG_P25_EXTRAS=1: the emergency bit, unit-to-unit
# voice) against MMDVMHost's own LDU1 coder (GPL-2.0-or-later), host only (docs/plus.md "P25 emergency and
# unit-to-unit calls"):
#   git clone --depth 1 https://github.com/g4klx/MMDVMHost.git ../research/ref/MMDVMHost
# The reference sources are compiled from where they are, in build/p25lc-ref, and never copied into this tree.
# P25LC_REF_MMDVMHOST overrides the reference path.
set -u
F=$(cd "$(dirname "$0")/.." && pwd)
R=${P25LC_REF_MMDVMHOST:-$F/../research/ref/MMDVMHost}
W=$F/build/p25lc-ref
[ -f "$R/P25Data.cpp" ] && [ -f "$R/RS634717.cpp" ] || { echo "reference sources missing (see the header)"; exit 2; }
rm -rf "$W" && mkdir -p "$W/ref" && cd "$W" || exit 1
for n in P25Data P25Utils P25Trellis RS634717 Hamming Golay24128 CRC Utils; do cp "$R/$n.cpp" "$R/$n.h" ref/; done
cp "$R/P25Defines.h" "$R/Defines.h" "$R/RS.h" ref/
cp "$F/tests/plus/ref_stub/Log.h" ref/Log.h	# the reference logger needs a JSON library: a no-op stand-in
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined"
gcc -std=gnu99 -Wall -Wextra -Werror $SAN -DCONFIG_P25_EXTRAS=1 -c "$F/src/p25/p25.c" -I"$F/src/p25" -o p25.o || exit 1
g++ -std=c++17 -Wall -Wextra -Werror $SAN -DCONFIG_P25_EXTRAS=1 -I"$F/src/p25" -I"$F/tests/plus" -c \
    "$F/tests/plus/test_p25lc_ref.cpp" -o test.o || exit 1
g++ -std=c++17 $SAN -Iref -I"$F/tests/plus" "$F/tests/plus/p25lc_ref_wrap.cpp" ref/P25Data.cpp \
    ref/P25Utils.cpp ref/P25Trellis.cpp ref/RS634717.cpp ref/Hamming.cpp ref/Golay24128.cpp ref/CRC.cpp \
    "$F/tests/plus/ref_stub/utils_stub.cpp" "$F/tests/plus/ref_stub/utils_dump_stub.cpp" test.o p25.o \
    -o test_p25lc_ref || exit 1
./test_p25lc_ref
