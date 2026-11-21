#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# macOS package of the DDM32 Plus programmer: "DDM32 Plus.app" in a .dmg, and the app_rs update zip
# (DDM32Plus/DDM32Plus + DDM32Plus/ddm32). Runs on a Mac (the GitHub workflow's macos runner, on the rust-app branch).
# Everything lands in rust/dist/ (git-ignored). The app is signed ad hoc only ("codesign -s -"): no Developer ID, no
# notarisation, no secret. Builds and packs only: nothing is uploaded or published, no radio is touched.
#
#   rust/packaging/macos/build-macos.sh [--no-build] [--no-radio]
#
# Needs Xcode's command line tools (sips, iconutil, codesign, hdiutil) and python3.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust="$(cd "$here/../.." && pwd)"
dist="$rust/dist"
build=1 radio=1
for a in "$@"; do
    case "$a" in
        --no-build) build=0 ;;
        --no-radio) radio=0 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

version="$(sed -n 's/^version = "\(.*\)"$/\1/p' "$rust/Cargo.toml" | head -n1)"
[ -n "$version" ] || { echo "no version in rust/Cargo.toml" >&2; exit 1; }
short="${version%%-*}"   # CFBundleShortVersionString takes numbers only: 3.0.0-alpha.1 -> 3.0.0
arch="$(uname -m)"
[ "$arch" = arm64 ] && arch=aarch64   # the platform key uses Rust's name (std::env::consts::ARCH)

if [ "$build" = 1 ]; then
    features=()
    [ "$radio" = 1 ] && features=(--features ddm32-cli/radio,ddm32-gui/radio)
    (cd "$rust" && cargo build --release --locked -p ddm32-cli -p ddm32-gui "${features[@]}")
fi
bin="$rust/target/release"
for f in ddm32-gui ddm32; do
    [ -x "$bin/$f" ] || { echo "missing $bin/$f" >&2; exit 1; }
done
mkdir -p "$dist"

# --- the update zip (app_rs, platform macos-ARCH) ---
python3 "$rust/packaging/make_app_zip.py" "macos-$arch" "$bin" "$dist/DDM32Plus-$version-macos-$arch.zip"

# --- the .app ---
app="$dist/DDM32 Plus.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
install -m755 "$bin/ddm32-gui" "$app/Contents/MacOS/DDM32Plus"
install -m755 "$bin/ddm32" "$app/Contents/MacOS/ddm32"
sed -e "s/@VERSION@/$version/g" -e "s/@SHORT_VERSION@/$short/g" "$here/Info.plist" > "$app/Contents/Info.plist"
plutil -lint "$app/Contents/Info.plist"
cp "$rust/README.md" "$rust/THIRD_PARTY.md" "$app/Contents/Resources/"

# the icon: an iconset from the 256 px picture, then .icns
iconset="$dist/ddm32plus.iconset"
rm -rf "$iconset"
mkdir -p "$iconset"
for s in 16 32 128 256; do
    sips -z "$s" "$s" "$rust/packaging/art/ddm32plus.png" --out "$iconset/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2))
    if [ "$d" -le 256 ]; then
        sips -z "$d" "$d" "$rust/packaging/art/ddm32plus.png" --out "$iconset/icon_${s}x${s}@2x.png" >/dev/null
    fi
done
iconutil -c icns "$iconset" -o "$app/Contents/Resources/ddm32plus.icns"
rm -rf "$iconset"

# ad-hoc signature: Apple silicon runs no unsigned code; it is not a Developer ID (Gatekeeper still asks once)
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"

# --- the .dmg: the app and a link to /Applications ---
stage="$dist/dmg"
rm -rf "$stage"
mkdir -p "$stage"
cp -R "$app" "$stage/"
ln -s /Applications "$stage/Applications"
dmg="$dist/DDM32Plus-$version-macos-$arch.dmg"
rm -f "$dmg"
hdiutil create -volname "DDM32 Plus" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$dmg"
rm -rf "$stage"
echo "app: $app"
echo "dmg: $dmg"
