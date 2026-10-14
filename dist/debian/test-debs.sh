#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
# Installs the .deb packages from dist/out in a clean container and checks them:
#   bash dist/debian/test-debs.sh [image]        (default ubuntu:24.04, needs docker)
# Kernel headers of the container's distribution kernel are installed first,
# so the DKMS postinst builds the module for that kernel.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE=${1:-ubuntu:24.04}
OUT=${OUT:-$REPO/dist/out}
# mount a copy, not dist/out itself (see dist/linux/test-tarball.sh)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp "$OUT"/*.deb "$TMP/"
docker run --rm -i -v "$TMP:/debs:ro" "$IMAGE" bash -s <<'EOF'
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
. /etc/os-release
hdr=linux-headers-generic; [ "$ID" = debian ] && hdr=linux-headers-amd64
apt-get install -y -qq "$hdr" >/dev/null
echo "== apt install"
apt-get install -y -qq /debs/usb2cansocketcan-dkms_*.deb /debs/usb2can-triple-tools_*.deb /debs/savvycan-triple_*.deb \
    python3-serial 2>&1 | grep -i -E "error|usb2cansocketcan:" || true
dpkg -l usb2cansocketcan-dkms usb2can-triple-tools savvycan-triple | tail -3
echo "== dkms"
dkms status
for m in /lib/modules/*/updates/dkms/usb2cansocketcan.ko*; do modinfo "$m" | grep -E "^(version|vermagic)"; done
ls /usr/lib/udev/rules.d/90-usb2can-triple.rules /usr/lib/systemd/system/usb2can-triple@.service
echo "== tools"
re-demo /tmp/demo.log && canre_cli /tmp/demo.log capture_overview
triple-dump --help | head -2
canopen-sim --help | head -1
echo "== savvycan-triple (offscreen, 5 s)"
QT_QPA_PLATFORM=offscreen timeout 5 savvycan-triple >/tmp/sc.log 2>&1 && rc=0 || rc=$?
[ $rc = 124 ] && echo "still running after 5 s: ok" || { echo "exit $rc"; cat /tmp/sc.log; }
echo "== remove"
apt-get remove -y -qq usb2cansocketcan-dkms usb2can-triple-tools savvycan-triple >/dev/null
dkms status | grep usb2can || echo "dkms: module removed"
ls /usr/src | grep usb2can || echo "source removed"
EOF
