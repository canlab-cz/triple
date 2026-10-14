#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
"""Simulated vehicle traffic for trying the reverse engineering tools.

A minute of driving from an imaginary car whose layout is known, so the
results of the Signal Finder, the AI Assistant and tools/canre_cli can be
checked against the truth (doc/reverse-engineering.md). Standard library only.

    python3 tools/re_demo.py demo.log            # write a candump log
    python3 tools/re_demo.py --live can0         # send in real time (Linux)

The log loads in SavvyCAN (File > Load Log File) and replays with
`canplayer -I demo.log`.

Messages (the "answer key"; --print-dbc writes it as a DBC file):

  0C9  10 ms  engine: RPM (16 bit Motorola, bytes 0-1, 0.25 rpm), throttle
              (byte 2, 0.4 %), coolant temperature (byte 3, -40 degC),
              constants 00 3C, rolling counter (byte 6, low nibble),
              CRC-8 SAE J1850 over bytes 0-6 (byte 7)
  1A0  20 ms  wheel speeds: four 16-bit Intel values, 0.01 km/h
  2F0 100 ms  body: byte 0 flags (bit 0 driver door, bit 1 passenger door,
              bit 4 low beam, bit 5 left turn signal lamp), byte 1 constant
  3E8 100 ms  battery, multiplexed by byte 0 (0..3): page 0 pack voltage
              (bytes 1-2 Intel, 0.1 V) and current (bytes 3-4 Intel, signed,
              0.1 A); page 1 cell temperatures (bytes 1-4, -40 degC); page 2
              state of charge (byte 1, 0.5 %); page 3 constant serial number
  18FEF100 100 ms  J1939 CCVS (extended ID): vehicle speed (bytes 1-2
              Intel, 1/256 km/h), brake switch (byte 3 bits 4-5)

Timeline: 0-10 s idle, 10-25 s accelerating to about 80 km/h, 25-40 s
cruising (left turn signal 30-34 s), 40-50 s braking to a stop, 52-55 s
driver door open, low beam on from 5 s.
"""

import argparse
import math
import random
import socket
import struct
import sys
import time

DURATION = 60.0


def crc8_j1850(data):
    c = 0xFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x1D) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c ^ 0xFF


def speed_at(t):
    """Vehicle speed in km/h."""
    if t < 10:
        return 0.0
    if t < 25:
        return 80.0 * (1 - math.cos(math.pi * (t - 10) / 15)) / 2
    if t < 40:
        return 80.0 + 1.5 * math.sin((t - 25) / 2.0)
    if t < 50:
        return 80.0 * (50 - t) / 10
    return 0.0


def throttle_at(t):
    if 10 <= t < 25:
        return 35 + 30 * math.sin(math.pi * (t - 10) / 15)
    if 25 <= t < 40:
        return 18 + 3 * math.sin(t)
    return 0.0


def braking(t):
    return 40 <= t < 50


class Car:
    def __init__(self, seed):
        self.rnd = random.Random(seed)
        self.counter = 0
        self.mux = 0

    def engine(self, t):
        v = speed_at(t)
        rpm = 800 + v * 30 + self.rnd.uniform(-15, 15)
        thr = throttle_at(t)
        coolant = min(90.0, 25 + t * 1.6)
        d = bytearray(8)
        struct.pack_into(">H", d, 0, int(rpm * 4))
        d[2] = int(thr / 0.4)
        d[3] = int(coolant + 40)
        d[4] = 0x00
        d[5] = 0x3C
        d[6] = self.counter
        self.counter = (self.counter + 1) & 0x0F
        d[7] = crc8_j1850(d[:7])
        return bytes(d)

    def wheels(self, t):
        v = speed_at(t)
        vals = []
        for k in range(4):
            # the outer (right) wheels turn a little faster in the left turn
            w = v * (1 + 0.004 * (k % 2) * (1 if 30 <= t < 34 else 0))
            if v > 0:
                w += self.rnd.uniform(-0.05, 0.05)
            vals.append(max(0, int(w / 0.01)))
        return struct.pack("<4H", *vals)

    def body(self, t):
        f = 0
        if 52 <= t < 55:
            f |= 0x01
        if t >= 5:
            f |= 0x10
        if 30 <= t < 34 and int((t - 30) * 2) % 2 == 0:
            f |= 0x20
        return bytes([f, 0x80, 0, 0])

    def battery(self, t):
        page = self.mux
        self.mux = (self.mux + 1) % 4
        d = bytearray(8)
        d[0] = page
        if page == 0:
            current = -5.0 - throttle_at(t) * 3.0 + (60.0 if braking(t) else 0.0) + self.rnd.uniform(-1, 1)
            volts = 396.0 + current * 0.05 + self.rnd.uniform(-0.2, 0.2)
            struct.pack_into("<Hh", d, 1, int(volts * 10), int(current * 10))
        elif page == 1:
            for k in range(4):
                d[1 + k] = int(25 + t / 20 + k + 40)
        elif page == 2:
            d[1] = int((80 - t / 30) / 0.5)
        else:
            d[1:5] = bytes([0x12, 0x34, 0x56, 0x78])
        return bytes(d)

    def ccvs(self, t):
        d = bytearray(8)
        d[0] = 0xFF
        struct.pack_into("<H", d, 1, int(speed_at(t) * 256))
        d[3] = 0x10 if braking(t) else 0x00
        d[4:8] = b"\xFF\xFF\xFF\xFF"
        return bytes(d)


SCHEDULE = [  # (id, extended, period s, first send s, generator)
    (0x0C9, False, 0.010, 0.0003, "engine"),
    (0x1A0, False, 0.020, 0.0021, "wheels"),
    (0x2F0, False, 0.100, 0.0057, "body"),
    (0x3E8, False, 0.100, 0.0083, "battery"),
    (0x18FEF100, True, 0.100, 0.0112, "ccvs"),
]


def frames(seed=1):
    """(time s, id, extended, data) in time order."""
    car = Car(seed)
    jit = random.Random(seed + 1)
    out = []
    for ident, ext, period, first, gen in SCHEDULE:
        n = int((DURATION - first) / period) + 1
        for k in range(n):
            t = first + k * period + jit.uniform(-0.0002, 0.0002)
            out.append((t, k, ident, ext, gen))
    out.sort(key=lambda x: x[0])
    for t, _, ident, ext, gen in out:
        yield t, ident, ext, getattr(car, gen)(t)


DBC = """VERSION ""

NS_ :

BS_:

BU_: ECU

BO_ 201 Engine: 8 ECU
 SG_ RPM : 7|16@0+ (0.25,0) [0|16383.75] "rpm" Vector__XXX
 SG_ Throttle : 16|8@1+ (0.4,0) [0|100] "%" Vector__XXX
 SG_ Coolant : 24|8@1+ (1,-40) [-40|215] "degC" Vector__XXX
 SG_ Counter : 48|4@1+ (1,0) [0|15] "" Vector__XXX
 SG_ Checksum : 56|8@1+ (1,0) [0|255] "" Vector__XXX

BO_ 416 WheelSpeeds: 8 ECU
 SG_ WheelFL : 0|16@1+ (0.01,0) [0|655.35] "km/h" Vector__XXX
 SG_ WheelFR : 16|16@1+ (0.01,0) [0|655.35] "km/h" Vector__XXX
 SG_ WheelRL : 32|16@1+ (0.01,0) [0|655.35] "km/h" Vector__XXX
 SG_ WheelRR : 48|16@1+ (0.01,0) [0|655.35] "km/h" Vector__XXX

BO_ 752 Body: 4 ECU
 SG_ DriverDoor : 0|1@1+ (1,0) [0|1] "" Vector__XXX
 SG_ PassengerDoor : 1|1@1+ (1,0) [0|1] "" Vector__XXX
 SG_ LowBeam : 4|1@1+ (1,0) [0|1] "" Vector__XXX
 SG_ TurnLeft : 5|1@1+ (1,0) [0|1] "" Vector__XXX

BO_ 1000 Battery: 8 ECU
 SG_ Page M : 0|8@1+ (1,0) [0|3] "" Vector__XXX
 SG_ PackVoltage m0 : 8|16@1+ (0.1,0) [0|6553.5] "V" Vector__XXX
 SG_ PackCurrent m0 : 24|16@1- (0.1,0) [-3276.8|3276.7] "A" Vector__XXX
 SG_ CellTemp1 m1 : 8|8@1+ (1,-40) [-40|215] "degC" Vector__XXX
 SG_ CellTemp2 m1 : 16|8@1+ (1,-40) [-40|215] "degC" Vector__XXX
 SG_ CellTemp3 m1 : 24|8@1+ (1,-40) [-40|215] "degC" Vector__XXX
 SG_ CellTemp4 m1 : 32|8@1+ (1,-40) [-40|215] "degC" Vector__XXX
 SG_ StateOfCharge m2 : 8|8@1+ (0.5,0) [0|100] "%" Vector__XXX
 SG_ SerialNumber m3 : 8|32@1+ (1,0) [0|4294967295] "" Vector__XXX

BO_ 2566844672 CCVS: 8 ECU
 SG_ VehicleSpeed : 8|16@1+ (0.00390625,0) [0|250.996] "km/h" Vector__XXX
 SG_ BrakeSwitch : 28|2@1+ (1,0) [0|3] "" Vector__XXX
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log", nargs="?", help="candump log file to write")
    ap.add_argument("--live", metavar="IFACE", help="send on a SocketCAN interface in real time instead")
    ap.add_argument("--iface", default="can0", help="interface name written into the log (default can0)")
    ap.add_argument("--seed", type=int, default=1, help="random seed (default 1)")
    ap.add_argument("--print-dbc", action="store_true", help="print the answer key as a DBC file and exit")
    args = ap.parse_args()

    if args.print_dbc:
        sys.stdout.write(DBC)
        return
    if args.live:
        s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        s.bind((args.live,))
        start = time.monotonic()
        for t, ident, ext, data in frames(args.seed):
            delay = start + t - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            can_id = ident | (socket.CAN_EFF_FLAG if ext else 0)
            pkt = struct.pack("=IB3x8s", can_id, len(data), data.ljust(8, b"\0"))
            while True:
                try:
                    s.send(pkt)
                    break
                except OSError:          # ENOBUFS: the queue is full, retry
                    time.sleep(0.001)
        return
    if not args.log:
        ap.error("give a log file name or --live IFACE")
    with open(args.log, "w") as f:
        for t, ident, ext, data in frames(args.seed):
            f.write("(%.6f) %s %s#%s\n" % (t, args.iface, ("%08X" if ext else "%03X") % ident, data.hex().upper()))


if __name__ == "__main__":
    main()
