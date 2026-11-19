#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# DM32 Plus: check src/ysf against the GPL-2.0-or-later reference implementations, host only (docs/plus.md).
#   git clone --depth 1 https://github.com/g4klx/MMDVMHost.git      ../research/ref/MMDVMHost
#   git clone --depth 1 https://github.com/juribeparada/MMDVM_CM.git ../research/ref/MMDVM_CM
# Run from this tree after a --plus build (it uses the transformed RadioFoundry AMBE FEC). The reference sources are
# compiled from where they are and never copied into this tree.
set -u
F=$(cd "$(dirname "$0")/.." && pwd)
R=${YSF_REF_MMDVMHOST:-$F/../research/ref/MMDVMHost}
C=${YSF_REF_MMDVMCM:-$F/../research/ref/MMDVM_CM/YSF2DMR}
RF=$F/build/radiofoundry-p25
W=$F/build/ysf-ref
[ -f "$R/YSFFICH.cpp" ] && [ -f "$C/ModeConv.cpp" ] || { echo "reference sources missing (see the header)"; exit 2; }
rm -rf "$W" && mkdir -p "$W/ref" && cd "$W" || exit 1
for n in YSFFICH YSFConvolution Golay24128 CRC Utils; do cp "$R/$n.cpp" "$R/$n.h" ref/; done
cp "$R/YSFDefines.h" "$R/Defines.h" ref/
cp "$F/tests/plus/ref_stub/Log.h" ref/Log.h	# the reference logger needs a JSON library: a no-op stand-in
python3 - "$C/ModeConv.cpp" > ref/ysf_ref_tables.h <<'PY'
import re, sys
src = open(sys.argv[1]).read()
for name in ("PRNG_TABLE", "DMR_A_TABLE", "DMR_B_TABLE", "DMR_C_TABLE"):
    m = re.search(r"const unsigned int %s\[\]\s*=\s*\{(.*?)\};" % name, src, re.S)
    print("static const unsigned int %s[] = {%s};" % (name, m.group(1)))
PY
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined"
gcc -std=gnu99 -Wall -Wextra $SAN -c "$F/src/ysf/ysf.c" -I"$F/src" -o ysf.o || exit 1
gcc -std=gnu99 $SAN -c "$RF/core/dmr/ambe/ambeFec.c" -I"$RF/core" -I"$RF/hal" -o ambefec.o || exit 1
g++ -std=c++11 $SAN -Iref -I"$F/src" "$F/tests/plus/test_ysf_ref.cpp" ref/YSFFICH.cpp ref/YSFConvolution.cpp \
    ref/Golay24128.cpp ref/CRC.cpp "$F/tests/plus/ref_stub/utils_stub.cpp" ysf.o -o test_ysf_ref || exit 1
g++ -std=c++11 $SAN -Iref -I"$F/src" -I"$RF/core" "$F/tests/plus/test_ysf_ambe_ref.cpp" ref/Golay24128.cpp \
    "$F/tests/plus/ref_stub/utils_stub.cpp" ysf.o ambefec.o -o test_ysf_ambe_ref || exit 1
./test_ysf_ref && ./test_ysf_ambe_ref
