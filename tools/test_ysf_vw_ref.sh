#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# DM32 Plus: check src/ysf/ysf_vw.c (YSF Voice FR mode) against the GPL-2.0-or-later reference implementations, host
# only, under ASan/UBSan (docs/plus.md). Also runs the self test tests/plus/test_ysf_vw.c.
#   git clone --depth 1 https://github.com/g4klx/MMDVMHost.git      ../research/ref/MMDVMHost
#   git clone --depth 1 https://github.com/juribeparada/MMDVM_CM.git ../research/ref/MMDVM_CM
# The reference sources are copied into a scratch folder (YSF_VW_WORK, default $HOME/agent-vw), never into this tree.
set -u
F=$(cd "$(dirname "$0")/.." && pwd)
R=${YSF_REF_MMDVMHOST:-$F/../research/ref/MMDVMHost}
C=${YSF_REF_MMDVMCM:-$F/../research/ref/MMDVM_CM/YSF2P25}
W=${YSF_VW_WORK:-$HOME/agent-vw}
[ -f "$R/YSFPayload.cpp" ] && [ -f "$R/AMBEFEC.cpp" ] && [ -f "$C/ModeConv.cpp" ] || {
	echo "reference sources missing (see the header)"; exit 2; }
rm -rf "$W" && mkdir -p "$W/ref" && cd "$W" || exit 1
for n in YSFPayload AMBEFEC YSFConvolution Golay24128 Hamming CRC Utils; do cp "$R/$n.cpp" "$R/$n.h" ref/; done
cp "$R/YSFDefines.h" "$R/Defines.h" ref/
cp "$F/tests/plus/ref_stub/Log.h" ref/Log.h	# the reference logger needs a JSON library: a no-op stand-in
python3 - "$R/AMBEFEC.cpp" "$C/ModeConv.cpp" > ref/vw_ref_tables.h <<'PY'
import re, sys
for path, name in zip(sys.argv[1:], ("IMBE_INTERLEAVE_HOST", "IMBE_INTERLEAVE_CM")):
    m = re.search(r"const unsigned int IMBE_INTERLEAVE\[\]\s*=\s*\{(.*?)\};", open(path).read(), re.S)
    print("static const unsigned int %s[144] = {%s};" % (name, m.group(1)))
PY
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
OURS="$F/src/ysf/ysf_vw.c $F/src/ysf/ysf.c $F/src/p25/p25.c"
gcc -std=gnu99 -Wall -Wextra -Werror $SAN -I"$F/src" -I"$F/src/p25" "$F/tests/plus/test_ysf_vw.c" $OURS \
    -o test_ysf_vw || exit 1
for c in $OURS; do gcc -std=gnu99 -Wall -Wextra -Werror $SAN -I"$F/src" -I"$F/src/p25" -c "$c" \
    -o "$(basename "$c" .c).o" || exit 1; done
g++ -std=c++11 -Wall -Wextra $SAN -Iref -I"$F/src" -I"$F/src/p25" "$F/tests/plus/test_ysf_vw_ref.cpp" \
    ref/YSFPayload.cpp ref/AMBEFEC.cpp ref/YSFConvolution.cpp ref/Golay24128.cpp ref/Hamming.cpp ref/CRC.cpp \
    "$F/tests/plus/ref_stub/utils_stub.cpp" ysf_vw.o ysf.o p25.o -o test_ysf_vw_ref || exit 1
./test_ysf_vw && ./test_ysf_vw_ref
