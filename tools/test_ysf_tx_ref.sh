#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# DM32 Plus: check src/ysf/ysf_tx.c (YSF Voice FR transmit framing) against the GPL-2.0-or-later MMDVMHost, host only,
# under ASan/UBSan (docs/ysf-nxdn-tx.md). Also runs the self test tests/plus/test_ysf_tx.c. Nothing transmits.
#   git clone --depth 1 https://github.com/g4klx/MMDVMHost.git ../research/ref/MMDVMHost
# The reference sources are copied into a scratch folder (YSF_TX_WORK, default $HOME/agent-ysftx), never into this tree.
set -u
F=$(cd "$(dirname "$0")/.." && pwd)
R=${YSF_REF_MMDVMHOST:-$F/../research/ref/MMDVMHost}
W=${YSF_TX_WORK:-$HOME/agent-ysftx}
[ -f "$R/YSFPayload.cpp" ] && [ -f "$R/YSFFICH.cpp" ] && [ -f "$R/AMBEFEC.cpp" ] || {
	echo "reference sources missing (see the header)"; exit 2; }
rm -rf "$W" && mkdir -p "$W/ref" && cd "$W" || exit 1
for n in YSFPayload YSFFICH AMBEFEC YSFConvolution Golay24128 Hamming CRC Utils; do cp "$R/$n.cpp" "$R/$n.h" ref/; done
cp "$R/YSFDefines.h" "$R/Defines.h" ref/
cp "$F/tests/plus/ref_stub/Log.h" ref/Log.h	# the reference logger needs a JSON library: a no-op stand-in
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
OURS="$F/src/ysf/ysf_tx.c $F/src/ysf/ysf_vw.c $F/src/ysf/ysf.c $F/src/p25/p25.c"
gcc -std=gnu99 -Wall -Wextra -Werror $SAN -I"$F/src" -I"$F/src/p25" "$F/tests/plus/test_ysf_tx.c" $OURS \
    -o test_ysf_tx || exit 1
for c in $OURS; do gcc -std=gnu99 -Wall -Wextra -Werror $SAN -I"$F/src" -I"$F/src/p25" -c "$c" \
    -o "$(basename "$c" .c).o" || exit 1; done
g++ -std=c++11 -Wall -Wextra $SAN -Iref -I"$F/src" -I"$F/src/p25" "$F/tests/plus/test_ysf_tx_ref.cpp" \
    ref/YSFPayload.cpp ref/YSFFICH.cpp ref/AMBEFEC.cpp ref/YSFConvolution.cpp ref/Golay24128.cpp ref/Hamming.cpp \
    ref/CRC.cpp "$F/tests/plus/ref_stub/utils_stub.cpp" ysf_tx.o ysf_vw.o ysf.o p25.o -o test_ysf_tx_ref || exit 1
./test_ysf_tx && ./test_ysf_tx_ref
