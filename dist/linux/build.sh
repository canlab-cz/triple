#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
# Generic Linux distribution: dist/out/usb2can-triple-<version>-linux-<arch>.tar.gz
#
# Contains the driver source with an installer (DKMS or plain make), the
# udev rule and systemd unit, the attach scripts, SavvyCAN with its Qt 5
# libraries bundled (runs without Qt installed; needs glibc 2.35 or newer,
# i.e. Ubuntu 22.04, Debian 12, Fedora 36 or later), the command line
# tools and the documentation.
#
# Build on Ubuntu 22.04 (or WSL2 with it) with:
#   sudo apt install build-essential git python3 qtbase5-dev qt5-qmake libqt5serialbus5-dev \
#        libqt5serialport5-dev qtdeclarative5-dev qttools5-dev libqt5websockets5-dev libqt5opengl5-dev
#   bash dist/linux/build.sh
. "$(dirname "$0")/common.sh"

NAME="usb2can-triple-$VERSION-linux-$(uname -m)"
STAGE="$WORK/stage-linux/$NAME"

build_savvycan
build_canre

log "staging $NAME"
rm -rf "$WORK/stage-linux"
mkdir -p "$STAGE"
copy_driver_source "$STAGE/driver"
mkdir -p "$STAGE/scripts" "$STAGE/etc"
cp "$REPO"/scripts/*.sh "$STAGE/scripts/"
cp -r "$REPO"/etc/udev "$REPO"/etc/systemd "$STAGE/etc/"
copy_python_tools "$STAGE/tools"
cp "$WORK/canre_cli" "$STAGE/tools/"
copy_docs "$STAGE/doc"
cp "$REPO/README.md" "$STAGE/README.md"          # doc/ mirrors the repository: links work
rm "$STAGE/doc/README.md"
cp "$REPO/LICENSE" "$STAGE/"
cp -r "$REPO/LICENSES" "$STAGE/"
cp "$(dirname "$0")/install.sh" "$STAGE/"
chmod +x "$STAGE/install.sh" "$STAGE"/scripts/*.sh "$STAGE"/tools/*.py

# ---- SavvyCAN with the Qt libraries it needs -----------------------------
log "bundling SavvyCAN"
APP="$STAGE/savvycan"
mkdir -p "$APP/lib" "$APP/plugins"
cp "$WORK/savvycan/build/SavvyCAN" "$APP/"
QTPLUG=$(qmake -query QT_INSTALL_PLUGINS)
for d in xcbglintegrations imageformats iconengines platforminputcontexts printsupport canbus; do
    [ -d "$QTPLUG/$d" ] && cp -r "$QTPLUG/$d" "$APP/plugins/"
done
# X11 (also under Wayland through XWayland), offscreen and minimal; not the
# framebuffer platforms, which need libinput
mkdir -p "$APP/plugins/platforms"
cp "$QTPLUG"/platforms/libqxcb.so "$QTPLUG"/platforms/libqoffscreen.so "$QTPLUG"/platforms/libqminimal.so "$APP/plugins/platforms/"
# Libraries that a desktop system may lack or have in another version: Qt,
# ICU and the helpers Qt depends on. glibc, libstdc++, X11 / xcb core,
# OpenGL, fontconfig, freetype, glib and the like come from the system.
bundle_wanted() {
    case "$1" in
        libQt5*|libicu*|libdouble-conversion*|libmd4c*|libpcre2-16*|libxcb-*|libxkbcommon*|libjpeg*) return 0 ;;
    esac
    return 1
}
pending=("$APP/SavvyCAN")
while IFS= read -r f; do pending+=("$f"); done < <(find "$APP/plugins" -name '*.so')
declare -A seen
while [ ${#pending[@]} -gt 0 ]; do
    f=${pending[0]}; pending=("${pending[@]:1}")
    while read -r name path; do
        [ -n "$path" ] && [ -f "$path" ] || continue
        bundle_wanted "$name" || continue
        [ -n "${seen[$name]:-}" ] && continue
        seen[$name]=1
        cp -L "$path" "$APP/lib/$name"
        pending+=("$APP/lib/$name")
    done < <(ldd "$f" 2>/dev/null | awk '/=>/ { print $1, $3 }')
done
strip --strip-unneeded "$APP"/lib/*.so* 2>/dev/null || true
cat > "$APP/qt.conf" <<'EOF'
[Paths]
Prefix = .
Plugins = plugins
EOF
cat > "$APP/savvycan" <<'EOF'
#!/bin/sh
# SavvyCAN with the bundled Qt libraries
here=$(dirname "$(readlink -f "$0")")
LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$here/SavvyCAN" "$@"
EOF
chmod +x "$APP/savvycan"
cp "$WORK/savvycan/icons/SavvyIcon.png" "$APP/savvycan.png"
cp "$WORK/savvycan/LICENSE" "$APP/LICENSE.SavvyCAN" 2>/dev/null || true
cp /usr/share/common-licenses/LGPL-3 "$APP/LICENSE.Qt-LGPL-3"
cat > "$APP/NOTICE" <<EOF
SavvyCAN (https://github.com/collin80/SavvyCAN, MIT license, LICENSE.SavvyCAN)
at upstream commit $SAVVYCAN_REV with the USB2CAN Triple additions of
https://github.com/canlab-cz/triple (gui/savvycan, MIT).

The Qt 5 libraries in lib/ and plugins/ are from Ubuntu's Qt 5 packages and
are used under the GNU LGPL version 3 (LICENSE.Qt-LGPL-3). Their source is
available from https://download.qt.io and from the Ubuntu archive
(qtbase-opensource-src and related source packages). You may replace them
with other builds of the same Qt version.
EOF
echo "bundled $(ls "$APP/lib" | wc -l) libraries, $(find "$APP/plugins" -name '*.so' | wc -l) plugins"

log "packing"
tar -C "$WORK/stage-linux" -czf "$OUT/$NAME.tar.gz" "$NAME"
ls -l "$OUT/$NAME.tar.gz"
