#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
# Installer of the generic Linux distribution of the USB2CAN Triple software.
#
#   sudo ./install.sh                 driver (DKMS if installed, else for the running kernel),
#                                     udev rule + systemd unit, SavvyCAN and tools in /opt
#   sudo ./install.sh --no-driver     only SavvyCAN and the tools
#   sudo ./install.sh --no-udev       no automatic ldattach when an adapter is plugged in
#   sudo ./install.sh --uninstall     remove everything this script installed
#
# The driver needs the kernel headers (linux-headers-$(uname -r) or kernel-devel),
# make and gcc; DKMS rebuilds it after kernel updates.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX=/opt/usb2can-triple
MOD=usb2cansocketcan
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$HERE/driver/dkms.conf")
DRIVER=1
UDEV=1
UNINSTALL=0

for a in "$@"; do
    case "$a" in
        --no-driver) DRIVER=0 ;;
        --no-udev) UDEV=0 ;;
        --uninstall) UNINSTALL=1 ;;
        --prefix=*) PREFIX=${a#--prefix=} ;;
        -h|--help) sed -n '4,13p' "$0"; exit 0 ;;
        *) echo "unknown option $a" >&2; exit 2 ;;
    esac
done
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)" >&2; exit 1; }

UDEV_RULE=/etc/udev/rules.d/90-usb2can-triple.rules
UNIT=/etc/systemd/system/usb2can-triple@.service
BIN=/usr/local/bin
SBIN=/usr/local/sbin
DESKTOP=/usr/share/applications/savvycan-triple.desktop

if [ $UNINSTALL = 1 ]; then
    if command -v dkms >/dev/null && dkms status -m $MOD 2>/dev/null | grep -q .; then
        dkms remove -m $MOD -v "$VER" --all || true
    fi
    rm -rf "/usr/src/$MOD-$VER"
    find /lib/modules -path "*/extra/$MOD.ko*" -delete 2>/dev/null || true
    depmod -a 2>/dev/null || true
    rm -f "$UDEV_RULE" "$UNIT" "$SBIN/triple-attach" "$SBIN/triple-detach" \
          "$BIN/savvycan-triple" "$BIN/canre_cli" "$BIN/triple-dump" "$BIN/canopen-sim" "$BIN/re-demo" "$DESKTOP"
    rm -rf "$PREFIX"
    command -v udevadm >/dev/null && udevadm control --reload || true
    command -v systemctl >/dev/null && systemctl daemon-reload || true
    echo "removed"
    exit 0
fi

if [ $DRIVER = 1 ]; then
    if command -v dkms >/dev/null; then
        echo "== driver $VER with DKMS"
        rm -rf "/usr/src/$MOD-$VER"
        cp -r "$HERE/driver" "/usr/src/$MOD-$VER"
        dkms status -m $MOD -v "$VER" 2>/dev/null | grep -q . || dkms add -m $MOD -v "$VER"
        dkms install -m $MOD -v "$VER" --force
    else
        echo "== driver $VER for kernel $(uname -r) (install dkms to rebuild it after kernel updates)"
        tmp=$(mktemp -d)
        cp -r "$HERE/driver" "$tmp/driver"
        make -C "$tmp/driver" >/dev/null
        install -D -m 644 "$tmp/driver/$MOD.ko" "/lib/modules/$(uname -r)/extra/$MOD.ko"
        rm -rf "$tmp"
        depmod -a
    fi
    install -D -m 755 "$HERE/scripts/triple-attach.sh" "$SBIN/triple-attach"
    install -D -m 755 "$HERE/scripts/triple-detach.sh" "$SBIN/triple-detach"
    if [ $UDEV = 1 ]; then
        echo "== udev rule and systemd unit (ldattach when an adapter is plugged in)"
        install -D -m 644 "$HERE/etc/udev/rules.d/90-usb2can-triple.rules" "$UDEV_RULE"
        install -D -m 644 "$HERE/etc/systemd/system/usb2can-triple@.service" "$UNIT"
        udevadm control --reload || true
        systemctl daemon-reload || true
    fi
fi

echo "== SavvyCAN and tools in $PREFIX"
rm -rf "$PREFIX"
mkdir -p "$PREFIX"
cp -r "$HERE/savvycan" "$HERE/tools" "$HERE/doc" "$HERE/README.md" "$HERE/LICENSE" "$HERE/LICENSES" "$PREFIX/"
ln -sf "$PREFIX/savvycan/savvycan" "$BIN/savvycan-triple"
ln -sf "$PREFIX/tools/canre_cli" "$BIN/canre_cli"
ln -sf "$PREFIX/tools/triple_dump.py" "$BIN/triple-dump"
ln -sf "$PREFIX/tools/canopen_sim.py" "$BIN/canopen-sim"
ln -sf "$PREFIX/tools/re_demo.py" "$BIN/re-demo"
mkdir -p "$(dirname "$DESKTOP")"
cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=SavvyCAN (USB2CAN Triple)
GenericName=CAN bus analyzer
Comment=CAN bus analyzer with the USB2CAN Triple connection and reverse engineering tools
Exec=$PREFIX/savvycan/savvycan
Icon=$PREFIX/savvycan/savvycan.png
Terminal=false
Categories=Development;Electronics;
EOF

echo
echo "done. Next steps:"
[ $DRIVER = 1 ] && echo "  sudo triple-attach ttyACM0        # or plug the adapter in (udev starts ldattach)"
echo "  savvycan-triple                   # SavvyCAN; add the Triple in Connection > Open Connection Window"
echo "  triple-dump needs pyserial (pip install pyserial or python3-serial)"
echo "  documentation: $PREFIX/doc"
