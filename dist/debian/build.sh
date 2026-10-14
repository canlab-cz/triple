#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
# Debian / Ubuntu packages in dist/out/:
#   usb2cansocketcan-dkms_<v>-1_all.deb    driver source for DKMS, udev rule, systemd unit, attach scripts
#   usb2can-triple-tools_<v>-1_<arch>.deb  canre_cli, triple-dump, canopen-sim, re-demo
#   savvycan-triple_<v>-1_<arch>.deb       SavvyCAN with the Triple connection and tools (Qt 5)
#
# Built on Ubuntu 22.04 they install on Ubuntu 22.04 and later and Debian 12
# and later. Build requirements as for dist/linux/build.sh plus dpkg-dev:
#   bash dist/debian/build.sh
. "$(dirname "$0")/../linux/common.sh"

REL=1
DEBVER="$VERSION-$REL"
MAINT="CANLAB s.r.o. <info@canlab.cz>"
HOMEPAGE="https://github.com/canlab-cz/triple"
STAGE="$WORK/stage-deb"

build_savvycan
build_canre
rm -rf "$STAGE"
mkdir -p "$STAGE"

# shlibs:Depends of the ELF files given, for the control file
shlibdeps() {
    local d
    d=$(mktemp -d)
    mkdir -p "$d/debian"
    printf 'Source: x\n\nPackage: x\nArchitecture: any\n' > "$d/debian/control"
    (cd "$d" && dpkg-shlibdeps -O -e "$@" 2>/dev/null) | sed -n 's/^shlibs:Depends=//p'
    rm -rf "$d"
}

copyright() {   # $1 = destination, $2 = license line of the package's own files
    cat > "$1" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: usb2can-triple
Source: $HOMEPAGE

Files: *
Copyright: 2026 CANLAB s.r.o.
License: $2
 The full license texts are in /usr/share/common-licenses/ (GPL-2) and in
 the source repository (LICENSES/MIT.txt).
EOF
}

# pkgdir, package, architecture, depends, recommends, description (first line), long description
control() {
    local size
    size=$(du -sk --exclude=DEBIAN "$1" | cut -f1)
    cat > "$1/DEBIAN/control" <<EOF
Package: $2
Version: $DEBVER
Architecture: $3
Maintainer: $MAINT
Installed-Size: $size
Depends: $4
Recommends: $5
Section: misc
Priority: optional
Homepage: $HOMEPAGE
Description: $6
$7
EOF
    (cd "$1" && find . -path ./DEBIAN -prune -o -type f -print0 | sort -z | xargs -0 md5sum | sed 's| \./| |' > DEBIAN/md5sums)
}

finish() {   # pkgdir
    find "$1" -type d -exec chmod 755 {} +
    dpkg-deb --root-owner-group -Zxz --build "$1" "$OUT/" >/dev/null
}

# ---- usb2cansocketcan-dkms ------------------------------------------------
log "usb2cansocketcan-dkms $DEBVER"
P="$STAGE/usb2cansocketcan-dkms"
mkdir -p "$P/DEBIAN"
copy_driver_source "$P/usr/src/usb2cansocketcan-$VERSION"
sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$VERSION\"/" "$P/usr/src/usb2cansocketcan-$VERSION/dkms.conf"
install -D -m 644 "$REPO/etc/udev/rules.d/90-usb2can-triple.rules" "$P/usr/lib/udev/rules.d/90-usb2can-triple.rules"
install -D -m 644 "$REPO/etc/systemd/system/usb2can-triple@.service" "$P/usr/lib/systemd/system/usb2can-triple@.service"
install -D -m 755 "$REPO/scripts/triple-attach.sh" "$P/usr/sbin/triple-attach"
install -D -m 755 "$REPO/scripts/triple-detach.sh" "$P/usr/sbin/triple-detach"
mkdir -p "$P/usr/share/doc/usb2cansocketcan-dkms"
copyright "$P/usr/share/doc/usb2cansocketcan-dkms/copyright" "GPL-2 (driver/triple.c), MIT (protocol codec, scripts)"
cp "$REPO/doc/linux-driver.md" "$REPO/doc/protocol.md" "$P/usr/share/doc/usb2cansocketcan-dkms/"
cat > "$P/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
NAME=usb2cansocketcan
VER=$VERSION
if [ "\$1" = configure ]; then
    dkms status -m \$NAME -v \$VER 2>/dev/null | grep -q . || dkms add -m \$NAME -v \$VER
    # every installed kernel that has its headers; dkms takes care of later kernels
    for k in /lib/modules/*; do
        kv=\$(basename "\$k")
        [ -e "\$k/build" ] || continue
        dkms install -m \$NAME -v \$VER -k "\$kv" || echo "usb2cansocketcan: could not build for \$kv" >&2
    done
    udevadm control --reload 2>/dev/null || true
    systemctl daemon-reload 2>/dev/null || true
fi
exit 0
EOF
cat > "$P/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
case "\$1" in
    remove|upgrade|deconfigure) dkms remove -m usb2cansocketcan -v $VERSION --all >/dev/null 2>&1 || true ;;
esac
exit 0
EOF
cat > "$P/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
udevadm control --reload 2>/dev/null || true
systemctl daemon-reload 2>/dev/null || true
exit 0
EOF
chmod 755 "$P"/DEBIAN/postinst "$P"/DEBIAN/prerm "$P"/DEBIAN/postrm
control "$P" usb2cansocketcan-dkms all "dkms, util-linux, kmod" "can-utils" \
    "SocketCAN driver for the CANLAB USB2CAN Triple (DKMS)" \
" Kernel module (tty line discipline 29) that turns a USB2CAN Triple adapter
 into three SocketCAN interfaces: two classic CAN ports and one CAN FD port,
 with hardware timestamps, bus state reporting and transmit flow control.
 The module is built by DKMS for every kernel with headers installed.
 .
 A udev rule and a systemd unit attach the line discipline when an adapter
 is plugged in; triple-attach / triple-detach do it by hand."
finish "$P"

# ---- usb2can-triple-tools -------------------------------------------------
log "usb2can-triple-tools $DEBVER"
P="$STAGE/usb2can-triple-tools"
mkdir -p "$P/DEBIAN" "$P/usr/bin" "$P/usr/share/usb2can-triple"
install -m 755 "$WORK/canre_cli" "$P/usr/bin/canre_cli"
copy_python_tools "$P/usr/share/usb2can-triple"
for t in triple-dump:triple_dump.py canopen-sim:canopen_sim.py re-demo:re_demo.py; do
    printf '#!/bin/sh\nexec python3 /usr/share/usb2can-triple/%s "$@"\n' "${t#*:}" > "$P/usr/bin/${t%%:*}"
    chmod 755 "$P/usr/bin/${t%%:*}"
done
mkdir -p "$P/usr/share/doc/usb2can-triple-tools"
copyright "$P/usr/share/doc/usb2can-triple-tools/copyright" "MIT"
cp "$REPO/doc/reverse-engineering.md" "$P/usr/share/doc/usb2can-triple-tools/"
control "$P" usb2can-triple-tools "$ARCH" "python3, $(shlibdeps "$P/usr/bin/canre_cli")" "python3-serial" \
    "Command line tools for the CANLAB USB2CAN Triple and CAN reverse engineering" \
" triple-dump talks to a USB2CAN Triple over its serial port without the
 kernel driver (needs python3-serial). canopen-sim simulates a CANopen node on
 a SocketCAN interface. re-demo writes simulated vehicle traffic with a known
 layout. canre_cli runs the reverse engineering analysis of SavvyCAN's
 Signal Finder and AI Assistant on a candump log."
finish "$P"

# ---- savvycan-triple ------------------------------------------------------
log "savvycan-triple $DEBVER"
P="$STAGE/savvycan-triple"
mkdir -p "$P/DEBIAN" "$P/usr/bin" "$P/usr/lib/savvycan-triple"
install -m 755 "$WORK/savvycan/build/SavvyCAN" "$P/usr/lib/savvycan-triple/SavvyCAN"
ln -s ../lib/savvycan-triple/SavvyCAN "$P/usr/bin/savvycan-triple"
install -D -m 644 "$WORK/savvycan/icons/SavvyIcon.png" "$P/usr/share/pixmaps/savvycan-triple.png"
install -d "$P/usr/share/applications"
cat > "$P/usr/share/applications/savvycan-triple.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=SavvyCAN (USB2CAN Triple)
GenericName=CAN bus analyzer
Comment=CAN bus analyzer with the USB2CAN Triple connection and reverse engineering tools
Exec=savvycan-triple
Icon=savvycan-triple
Terminal=false
Categories=Development;Electronics;
Keywords=CAN;CAN FD;canbus;CANopen;reverse engineering;
EOF
D="$P/usr/share/doc/savvycan-triple"
mkdir -p "$D"
copy_docs "$D"
rm "$D/README.md"
cat > "$D/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: SavvyCAN with the USB2CAN Triple additions
Source: https://github.com/collin80/SavvyCAN (commit $SAVVYCAN_REV), $HOMEPAGE

Files: *
Copyright: Collin Kidder and the SavvyCAN contributors;
 2026 CANLAB s.r.o. (USB2CAN Triple connection and tool windows)
License: MIT
EOF
sed 's/^$/./; s/^/ /' "$WORK/savvycan/LICENSE" >> "$D/copyright"
control "$P" savvycan-triple "$ARCH" "$(shlibdeps "$P/usr/lib/savvycan-triple/SavvyCAN")" "libqt5serialbus5-plugins" \
    "CAN bus analyzer (SavvyCAN) with USB2CAN Triple support" \
" SavvyCAN with a native connection for the CANLAB USB2CAN Triple (three
 buses, CAN FD, hardware timestamps) and additional tools: Bus Statistics,
 CANopen (SDO, PDOs from EDS, block transfer, LSS), Data Panel, Response
 Meter, CAN Calculator, PP2CAN log files, Print Log, and for reverse
 engineering a Signal Finder and an AI Assistant (Claude, your API key).
 The command is savvycan-triple."
finish "$P"

ls -l "$OUT"/*.deb
