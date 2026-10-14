#!/bin/sh
# Runs inside a container that has kernel headers installed.
# The repository is mounted read-only at /src.
KDIR=$(ls -d /lib/modules/*/build /usr/src/kernels/* 2>/dev/null | head -1)
echo "== kernel headers: $KDIR"
rm -rf /tmp/t && mkdir -p /tmp/t && cp -r /src/driver /tmp/t/
cd /tmp/t/driver && make KERNEL_SRC="$KDIR" 2>&1
rc=$?
echo "== driver build exit: $rc"
[ -f /tmp/t/usb2cansocketcan.ko ] && modinfo /tmp/t/usb2cansocketcan.ko | grep -E "^(version|alias|vermagic|parm)"
exit $rc
