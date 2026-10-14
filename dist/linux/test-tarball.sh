#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
# Smoke test of the generic Linux tarball in a clean container:
#   bash dist/linux/test-tarball.sh [image]        (default ubuntu:24.04, needs docker)
# Installs only what a desktop system has anyway (X11, OpenGL, fontconfig,
# glib), checks that every bundled binary finds its libraries, starts
# SavvyCAN offscreen, runs install.sh --no-driver and builds the driver
# against the container's kernel headers.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE=${1:-ubuntu:24.04}
OUT=${OUT:-$REPO/dist/out}
# mount a copy: a bind mount of a file under /mnt/c can leave WSL unable to
# recreate that path later (Docker Desktop)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp "$(ls "$OUT"/usb2can-triple-*-linux-*.tar.gz | tail -1)" "$TMP/pkg.tar.gz"
docker run --rm -i -v "$TMP:/pkg:ro" "$IMAGE" bash -s <<'EOF'
set -eu
export DEBIAN_FRONTEND=noninteractive
if command -v apt-get >/dev/null; then
    apt-get update -qq
    apt-get install -y -qq libgl1 libegl1 libx11-6 libx11-xcb1 libxcb1 libfontconfig1 libfreetype6 libglib2.0-0 \
        libharfbuzz0b libpng16-16 libgssapi-krb5-2 libcups2 libdbus-1-3 libxrender1 libsm6 libice6 \
        python3 make gcc kmod >/dev/null
    . /etc/os-release; if [ "$ID" = debian ]; then apt-get install -y -qq linux-headers-amd64 >/dev/null; else apt-get install -y -qq linux-headers-generic >/dev/null; fi
else
    dnf install -y -q mesa-libGL mesa-libEGL libX11 libX11-xcb libxcb fontconfig freetype glib2 harfbuzz libpng \
        krb5-libs cups-libs dbus-libs libXrender libSM libICE python3 make gcc kmod kernel-devel findutils >/dev/null
fi
cd /tmp && tar xzf /pkg/pkg.tar.gz && cd usb2can-triple-*
echo "== missing libraries"
missing=0
for f in savvycan/SavvyCAN $(find savvycan/plugins savvycan/lib -name '*.so*') tools/canre_cli; do
    m=$(LD_LIBRARY_PATH=savvycan/lib ldd "$f" | grep "not found" || true)
    [ -n "$m" ] && { echo "$f: $m"; missing=1; }
done
[ $missing = 0 ] && echo none
echo "== SavvyCAN starts (offscreen, 5 s)"
QT_QPA_PLATFORM=offscreen timeout 5 savvycan/savvycan >/tmp/sc.log 2>&1 && rc=0 || rc=$?
[ $rc = 124 ] && echo "still running after 5 s: ok" || { echo "exit $rc"; cat /tmp/sc.log; }
echo "== tools"
python3 tools/re_demo.py /tmp/demo.log && tools/canre_cli /tmp/demo.log capture_overview
echo "== install.sh --no-driver"
./install.sh --no-driver | tail -3
QT_QPA_PLATFORM=offscreen timeout 3 savvycan-triple >/dev/null 2>&1 && rc=0 || rc=$?
[ $rc = 124 ] && echo "savvycan-triple runs: ok" || echo "savvycan-triple exit $rc"
canre_cli /tmp/demo.log list_ids | head -2
./install.sh --uninstall
echo "== driver build against the container's headers"
kdir=$(ls -d /usr/src/linux-headers-*-generic /usr/src/linux-headers-*-amd64 /usr/src/kernels/* 2>/dev/null | head -1)
make -C driver KERNEL_SRC="$kdir" >/dev/null && modinfo driver/usb2cansocketcan.ko | grep -E "^(version|vermagic)"
EOF
