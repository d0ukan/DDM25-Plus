#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Copyright (c) 2026 Open25-DM32 Contributors
"""Check the Classic product policy and application-only package locally."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import zipfile

from build_classic import PROFILE, ROOT, classic_profile, package_application, product_header

DEFAULT_PROFILE = classic_profile()
APP_BASE, APP_END = DEFAULT_PROFILE["app_base"], DEFAULT_PROFILE["app_end"]


def packaging():
    with tempfile.TemporaryDirectory() as directory:
        p = Path(directory)
        app = p / "app.bin"
        newlib = p / "COPYING.NEWLIB"
        newlib.write_text("Local toolchain notice fixture\n")
        for count in (1, APP_END - APP_BASE):
            app.write_bytes(bytes(count))
            receipt = package_application(app, p / "app.zip", newlib_notice=newlib)
            package_application(app, p / "again.zip", newlib_notice=newlib)
            assert (p / "app.zip").read_bytes() == (p / "again.zip").read_bytes()
            with zipfile.ZipFile(p / "app.zip") as archive:
                manifest = json.loads(archive.read("firmware.json"))
                assert manifest["applicationSha256"] == hashlib.sha256(app.read_bytes()).hexdigest()
                assert manifest["programming"] == {"schema": "rfkv-p25-contact-v1"}
                assert manifest["target"] == DEFAULT_PROFILE["target"]
                assert all(item.compress_type == zipfile.ZIP_STORED for item in archive.infolist())
                expected = {"application.bin", "firmware.json", "notices/LICENSES/newlib-COPYING.NEWLIB.txt"}
                expected |= {"notices/" + name for name in ("LICENSE", "AUTHORS", "CONTRIBUTORS", "core/mbe/THIRD_PARTY",
                             "platform/hrc7000/THIRD_PARTY", "third_party/FreeRTOS-Kernel/LICENSE.md")}
                expected |= {"notices/" + str(path.relative_to(ROOT)) for path in (ROOT / "LICENSES").glob("*.txt")}
                expected |= {"notices/fonts/" + name + ".txt" for name in ("spleen_5x8.inc", "jbmono_6x12.inc")}
                assert set(archive.namelist()) == expected
                assert archive.read("notices/LICENSES/newlib-COPYING.NEWLIB.txt") == newlib.read_bytes()
                assert b"Frederic Cambus" in archive.read("notices/fonts/spleen_5x8.inc.txt")
            assert receipt["application_range"][1] <= APP_END and not receipt["codec_range_written"]
        for count in (0, APP_END - APP_BASE + 1):
            app.write_bytes(bytes(count))
            try:
                package_application(app, p / "bad.zip")
            except ValueError:
                pass
            else:
                raise AssertionError("Accepted an empty or codec-overlapping application")
    assert all(PROFILE[k] == "y" for k in ("FM", "DMR", "P25", "NXDN", "HRC7000_NATIVE_RX", "CPS_RFKV_ONLY"))
    resolved = dict(line.split("=", 1) for line in subprocess.check_output(
        ["make", "-s", "BOARD=" + DEFAULT_PROFILE["board"], "VARIANT=classic", *["CONFIG_" + k + "=" + v for k, v in PROFILE.items()],
         "print-config"], cwd=ROOT, text=True).splitlines() if "=" in line)
    for flag, enabled in PROFILE.items():
        assert resolved[flag] == enabled, flag
    # A variant only disables upstream defaults. The product helper supplies required enables.
    variant = (ROOT / "config/variants/classic.mk").read_text()
    for flag in ("APRS", "P25_TRUNK", "DMR_TRUNK", "DMR_IP_GATEWAY", "HOTSPOT", "SFR", "NET_PPP"):
        assert "CONFIG_" + flag + " ?= n" in variant, flag
        assert resolved[flag] == "n", flag
    print("Classic application packaging: boundary, contents, notices and reproducibility passed")


def host_command(target):
    host = ROOT / "tests/host"
    output = subprocess.check_output(["make", "-Bn", target, "SANITIZE=1"], cwd=host, text=True)
    return host, shlex.split(next(line for line in output.splitlines() if " -o " + target + " " in line))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gallery", type=Path)
    args = parser.parse_args()
    packaging()
    out = ROOT / "build/classic-test/host"
    out.mkdir(parents=True, exist_ok=True)
    header = product_header(out / "gen")
    extra = ["-DCONFIG_RX_EXT_ALWAYS=1", "-DCONFIG_P25_TX_BAND_POLICY=1", "-DCONFIG_PRODUCT_CLASSIC=1", "-include", str(header)]
    host, command = host_command("uiFlowTest")
    command[command.index("uiFlowTest")] = str(out / "classicProductUiTest")
    command[command.index("uiFlowTest.c")] = "classicProductUiTest.c"
    subprocess.run([*command, *extra], cwd=host, check=True)
    subprocess.run([str(out / "classicProductUiTest")], cwd=host, check=True)
    if args.gallery:
        args.gallery.mkdir(parents=True, exist_ok=True)
        host, command = host_command("uiSim")
        command[command.index("uiSim")] = str(out / "uiSimClassic")
        subprocess.run([*command, *extra], cwd=host, check=True)
        subprocess.run([str(out / "uiSimClassic"), "--gallery", str(args.gallery / "classic")], cwd=host, check=True)


if __name__ == "__main__":
    main()
