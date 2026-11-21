#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux packages of the DDM32 Plus programmer: an AppImage (one file, runs on most distributions) and, with --deb, a
# Debian / Ubuntu package. Also the app_rs update zip (DDM32Plus/DDM32Plus + DDM32Plus/ddm32). Everything lands in
# rust/dist/ (git-ignored). Builds and packs only: nothing is signed, uploaded or published, no radio is touched.
#
#   rust/packaging/linux/build-linux.sh                # AppImage + zip (needs appimagetool, see below)
#   rust/packaging/linux/build-linux.sh --deb          # also the .deb (needs dpkg-deb)
#   rust/packaging/linux/build-linux.sh --appdir-only  # stop at the AppDir (no appimagetool needed)
#   options: --no-build (use target/release as it is), --no-radio (a build without serial port access)
#
# appimagetool: taken from $APPIMAGETOOL or PATH (https://github.com/AppImage/appimagetool/releases). This script never
# downloads it, so the tool that packs the program is one you chose and checked.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rust="$(cd "$here/../.." && pwd)"
dist="$rust/dist"
build=1 radio=1 deb=0 appimage=1
for a in "$@"; do
    case "$a" in
        --deb) deb=1 ;;
        --appdir-only) appimage=0 ;;
        --no-build) build=0 ;;
        --no-radio) radio=0 ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

version="$(sed -n 's/^version = "\(.*\)"$/\1/p' "$rust/Cargo.toml" | head -n1)"
arch="$(uname -m)"
case "$arch" in
    x86_64) debarch=amd64 ;;
    aarch64) debarch=arm64 ;;
    *) debarch="$arch" ;;
esac
[ -n "$version" ] || { echo "no version in rust/Cargo.toml" >&2; exit 1; }

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

# --- the update zip (app_rs, platform linux-ARCH) ---
python3 "$rust/packaging/make_app_zip.py" "linux-$arch" "$bin" "$dist/DDM32Plus-$version-linux-$arch.zip"

# --- the AppDir ---
appdir="$dist/AppDir"
rm -rf "$appdir"
install -Dm755 "$bin/ddm32-gui" "$appdir/usr/bin/DDM32Plus"
install -Dm755 "$bin/ddm32" "$appdir/usr/bin/ddm32"
install -Dm644 "$here/ddm32plus.desktop" "$appdir/usr/share/applications/ddm32plus.desktop"
install -Dm644 "$rust/packaging/art/ddm32plus.png" "$appdir/usr/share/icons/hicolor/256x256/apps/ddm32plus.png"
install -Dm644 "$rust/README.md" "$appdir/usr/share/doc/ddm32plus/README.md"
install -Dm644 "$rust/THIRD_PARTY.md" "$appdir/usr/share/doc/ddm32plus/THIRD_PARTY.md"
cp "$here/ddm32plus.desktop" "$appdir/ddm32plus.desktop"
cp "$rust/packaging/art/ddm32plus.png" "$appdir/ddm32plus.png"
ln -s ddm32plus.png "$appdir/.DirIcon"
cat > "$appdir/AppRun" <<'EOF'
#!/bin/sh
# The window by default; "AppRun ddm32 ARGS" runs the command line.
here="$(dirname "$(readlink -f "$0")")"
if [ "${1:-}" = "ddm32" ]; then shift; exec "$here/usr/bin/ddm32" "$@"; fi
exec "$here/usr/bin/DDM32Plus" "$@"
EOF
chmod 755 "$appdir/AppRun"
echo "AppDir: $appdir"

if [ "$appimage" = 1 ]; then
    tool="${APPIMAGETOOL:-$(command -v appimagetool || command -v appimagetool-"$arch".AppImage || true)}"
    if [ -z "$tool" ]; then
        echo "appimagetool not found: set APPIMAGETOOL=/path/to/appimagetool-$arch.AppImage (or use --appdir-only)" >&2
        exit 1
    fi
    out="$dist/DDM32Plus-$version-$arch.AppImage"
    # --no-appstream: no AppStream metadata is shipped; APPIMAGE_EXTRACT_AND_RUN lets the tool run without FUSE (CI)
    ARCH="$arch" APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --no-appstream "$appdir" "$out"
    echo "AppImage: $out"
fi

if [ "$deb" = 1 ]; then
    command -v dpkg-deb >/dev/null || { echo "dpkg-deb not found" >&2; exit 1; }
    # Debian orders 3.0.0~alpha.1 before 3.0.0, as SemVer does with 3.0.0-alpha.1
    debver="${version/-/\~}"
    root="$dist/deb"
    rm -rf "$root"
    install -Dm755 "$bin/ddm32-gui" "$root/opt/ddm32plus/DDM32Plus"
    install -Dm755 "$bin/ddm32" "$root/opt/ddm32plus/ddm32"
    mkdir -p "$root/usr/bin"
    ln -s /opt/ddm32plus/DDM32Plus "$root/usr/bin/DDM32Plus"
    ln -s /opt/ddm32plus/ddm32 "$root/usr/bin/ddm32"
    install -Dm644 "$here/ddm32plus.desktop" "$root/usr/share/applications/ddm32plus.desktop"
    install -Dm644 "$rust/packaging/art/ddm32plus.png" "$root/usr/share/icons/hicolor/256x256/apps/ddm32plus.png"
    install -Dm644 "$rust/README.md" "$root/usr/share/doc/ddm32plus/README.md"
    install -Dm644 "$rust/THIRD_PARTY.md" "$root/usr/share/doc/ddm32plus/THIRD_PARTY.md"
    size="$(du -sk "$root" | cut -f1)"
    mkdir -p "$root/DEBIAN"
    cat > "$root/DEBIAN/control" <<EOF
Package: ddm32plus
Version: $debver
Architecture: $debarch
Maintainer: d0ukan <81981301+d0ukan@users.noreply.github.com>
Installed-Size: $size
Depends: libc6, libfontconfig1, libgcc-s1
Recommends: libxkbcommon0, libwayland-client0 | libx11-6
Section: hamradio
Priority: optional
Homepage: https://github.com/d0ukan/DDM32-Plus
Description: DDM32 Plus programmer for the Baofeng DM-32UV
 Installs, updates and backs up the DDM32 Plus firmware over the programming
 cable. Backups stay in ~/.local/share/DDM32Plus/backups; removing the
 package never touches them. The serial port needs the user in the "dialout"
 group (the package does not change groups).
EOF
    out="$dist/ddm32plus_${debver}_${debarch}.deb"
    dpkg-deb --root-owner-group --build "$root" "$out"
    echo "deb: $out"
fi
