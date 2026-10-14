#!/bin/sh
# Manual alternative to the udev rule: attach one adapter and bring its
# three ports up with the README default bitrates.
#   sh scripts/triple-attach.sh ttyACM0
set -e
TTY=${1:-ttyACM0}
modprobe usb2cansocketcan 2>/dev/null || insmod "$(dirname "$0")/../usb2cansocketcan.ko"
ldattach 29 /dev/$TTY
sleep 0.5
# the three newest can interfaces belong to this adapter
PORTS=$(ls -d /sys/class/net/*/device 2>/dev/null | xargs -n1 dirname | xargs -n1 basename | while read n; do [ "$(cat /sys/class/net/$n/type)" = 280 ] && echo "$n $(cat /sys/class/net/$n/ifindex)"; done | sort -k2 -n | tail -3 | cut -d" " -f1)
set -- $PORTS
ip link set "$1" type can bitrate 250000
ip link set "$2" type can bitrate 500000
ip link set "$3" type can bitrate 125000 dbitrate 6700000 fd on
ip link set "$1" up; ip link set "$2" up; ip link set "$3" up
echo "$TTY: CAN1=$1 (250 kbit/s) CAN2=$2 (500 kbit/s) CAN3=$3 (FD 125k/6.7M)"
