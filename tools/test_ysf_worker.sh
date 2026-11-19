#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# DM32 Plus: YSF receive end to end through the actual P25 channel worker (tests/plus/test_ysf_worker.c), host only,
# ASan/UBSan. Compiles the real integration/radiofoundry/p25_lab.c (#included by the test), src/ysf, src/p25 and the
# transformed RadioFoundry tree's ambeFec.c; the native modem session and the DMR voice sink are mocked.
#
# Needs a prepared RadioFoundry tree (python3 tools/build_radiofoundry.py --plus, or --prepare-only) and the host IMBE
# codec library (python3 tools/test_receive_audio.py builds it). Overrides:
#   RF_TREE    transformed tree       (default: build/radiofoundry-p25)
#   CODEC_LIB  libdm32_imbe.a         (default: build/codec-speed-host/libdm32_imbe.a)
#   WORK       scratch output dir     (default: build/ysf-worker)
set -u
F=$(cd "$(dirname "$0")/.." && pwd)
RF=${RF_TREE:-$F/build/radiofoundry-p25}
LIB=${CODEC_LIB:-$F/build/codec-speed-host/libdm32_imbe.a}
W=${WORK:-$F/build/ysf-worker}
CC=${CC:-clang}
[ -f "$RF/core/dmr/ambe/ambeFec.c" ] || { echo "transformed RadioFoundry tree missing: $RF"; exit 2; }
[ -f "$LIB" ] || { echo "host IMBE codec library missing: $LIB"; exit 2; }
mkdir -p "$W" || exit 1
FLAGS=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined
       -fno-sanitize-recover=undefined -fno-omit-frame-pointer
       -I"$F/src/p25" -I"$F/src/codec" -I"$F/src" -I"$F/tests/audio/stubs" -I"$F/integration/radiofoundry"
       -I"$RF/core" -I"$RF/hal"
       -DCONFIG_P25_NATIVE=1 -DCONFIG_P25_BENCH=0 -DCONFIG_DEV_TX=1 -DCONFIG_P25_CHANNELS=1 -DCONFIG_DMR=1
       -DCONFIG_YSF=1)
OBJS=()
compile() {	# source [object name]
	local o="$W/${2:-$(basename "${1%.c}")}.o"
	"$CC" "${FLAGS[@]}" -c "$1" -o "$o" || exit 1
	OBJS+=("$o")
}
for n in p25 voice phy_stream selftest audio_fifo; do compile "$F/src/p25/$n.c"; done
compile "$F/src/ysf/ysf.c"
compile "$F/src/ysf/ysf_vw.c"
compile "$F/integration/radiofoundry/cpu_profile.c"
compile "$RF/core/debuglink/dl_fmt.c"
compile "$RF/core/radio/beep.c"
compile "$RF/core/dmr/ambe/ambeFec.c"
compile "$F/tests/plus/test_ysf_worker.c"
clang++ -fsanitize=address,undefined "${OBJS[@]}" "$LIB" -o "$W/test_ysf_worker" || exit 1
"$W/test_ysf_worker"
