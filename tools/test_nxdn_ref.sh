#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# DM32 Plus: check src/nxdn against MMDVMHost (GPL-2.0-or-later) and RadioFoundry's ambeFec, host only (docs/nxdn.md).
#   git clone --depth 1 https://github.com/g4klx/MMDVMHost.git ../research/ref/MMDVMHost
# Needs a RadioFoundry tree with core/dmr/ambe/ambeFec.c (after a --plus build: build/radiofoundry-p25; or set
# NXDN_REF_RF). The reference sources are compiled from where they are and never copied into this tree.
# NXDN_REF_OUT: where to build (default build/nxdn-ref).
set -u
F=$(cd "$(dirname "$0")/.." && pwd)
R=${NXDN_REF_MMDVMHOST:-$F/../research/ref/MMDVMHost}
RF=${NXDN_REF_RF:-$F/build/radiofoundry-p25}
W=${NXDN_REF_OUT:-$F/build/nxdn-ref}
[ -f "$R/NXDNSACCH.cpp" ] || { echo "MMDVMHost sources missing (see the header)"; exit 2; }
[ -f "$RF/core/dmr/ambe/ambeFec.c" ] || { echo "RadioFoundry ambeFec missing (see the header)"; exit 2; }
rm -rf "$W" && mkdir -p "$W/ref" && cd "$W" || exit 1
for n in NXDNLICH NXDNSACCH NXDNFACCH1 NXDNLayer3 NXDNAudio NXDNConvolution NXDNCRC Golay24128; do
	cp "$R/$n.cpp" "$R/$n.h" ref/
done
cp "$R/NXDNDefines.h" "$R/Defines.h" "$R/Utils.h" ref/
python3 - "$R/NXDNControl.cpp" "$R/NXDNAudio.cpp" > ref/nxdn_ref_tables.h <<'PY'
import re, sys
for path, name, kind in ((sys.argv[1], "SCRAMBLER", "unsigned char"), (sys.argv[2], "PRNG_TABLE", "unsigned int")):
    m = re.search(r"const unsigned (?:char|int) %s\[\]\s*=\s*\{(.*?)\};" % name, open(path).read(), re.S)
    print("static const %s %s[] = {%s};" % (kind, name, m.group(1)))
PY
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined"
for s in nxdn nxdn_demod nxdn_rx; do
	gcc -std=c11 -Wall -Wextra -Werror $SAN -c "$F/src/nxdn/$s.c" -I"$F/src" -o $s.o || exit 1
done
gcc -std=gnu99 $SAN -c "$RF/core/dmr/ambe/ambeFec.c" -I"$RF/core" -I"$RF/hal" -o ambefec.o || exit 1
# The reference itself without sanitizers: CNXDNFACCH1::encode and CNXDNSACCH::encode read one entry past their
# PUNCTURE_LIST (harmless there, fatal under ASan/UBSan)
REF=""
for n in NXDNLICH NXDNSACCH NXDNFACCH1 NXDNLayer3 NXDNAudio NXDNConvolution NXDNCRC Golay24128; do
	g++ -std=c++11 -O1 -g -Iref -c ref/$n.cpp -o ref/$n.o || exit 1
	REF="$REF ref/$n.o"
done
g++ -std=c++11 -O1 -g -Iref -c "$F/tests/plus/ref_stub/utils_stub.cpp" -o ref/utils_stub.o || exit 1
g++ -std=c++11 -Wall -Wextra -Werror $SAN -Iref -I"$F/src" -I"$RF/core" "$F/tests/plus/test_nxdn_ref.cpp" $REF \
    ref/utils_stub.o nxdn.o nxdn_demod.o nxdn_rx.o ambefec.o -o test_nxdn_ref || exit 1
./test_nxdn_ref
