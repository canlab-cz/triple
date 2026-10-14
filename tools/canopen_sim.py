#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
"""Minimal simulated CANopen node (CiA 301 subset) on a Linux SocketCAN interface.

For testing CANopen tools (e.g. the SavvyCAN CANopen window) without a real
device. Standard library only.

    python3 tools/canopen_sim.py can0 --node 5

Behaviour:
  * boot-up message, then heartbeat every 0x1017 ms (default 1000)
  * NMT: start, stop, pre-operational, reset node, reset communication
  * SDO server: expedited and segmented upload and download
  * abort 0x06020000 for unknown objects, 0x06010002 for read-only ones
  * writing 0xEE to 0x2000 sends an EMCY (0x5000 device hardware),
    writing 0x00 to 0x2000 afterwards sends "error reset"
"""
import argparse
import socket
import struct
import time

FMT = "=IB3x8s"  # struct can_frame: id, len, pad, data

NMT_STATES = {0x01: 0x05, 0x02: 0x04, 0x80: 0x7F}

ABORT_NO_OBJECT = 0x06020000
ABORT_READ_ONLY = 0x06010002
ABORT_TOGGLE = 0x05030000
ABORT_CMD = 0x05040001
ABORT_LENGTH = 0x06070010


class Node:
    def __init__(self, sock, node_id):
        self.sock = sock
        self.id = node_id
        self.state = 0x7F
        self.od = {}
        self.ro = set()
        self.reset_od()
        self.sdo = None          # active segmented transfer
        self.next_hb = 0.0
        self.emcy_active = False

    def reset_od(self):
        self.od = {
            (0x1000, 0): struct.pack("<I", 0x00020192),
            (0x1001, 0): b"\x00",
            (0x1008, 0): b"Triple CANopen test node",
            (0x1009, 0): b"HW 1.0",
            (0x100A, 0): b"SW 2.0.0-sim",
            (0x1017, 0): struct.pack("<H", 1000),
            (0x1018, 0): b"\x04",
            (0x1018, 1): struct.pack("<I", 0x000004D8),
            (0x1018, 2): struct.pack("<I", 0x0000F0D7),
            (0x1018, 3): struct.pack("<I", 0x00010002),
            (0x1018, 4): struct.pack("<I", 2604002),
            (0x2000, 0): b"\x00",
            (0x2001, 0): b"hello",
        }
        self.ro = {(0x1000, 0), (0x1008, 0), (0x1009, 0), (0x100A, 0), (0x1018, 0),
                   (0x1018, 1), (0x1018, 2), (0x1018, 3), (0x1018, 4)}

    # -- transport --------------------------------------------------------
    def send(self, can_id, data=b""):
        self.sock.send(struct.pack(FMT, can_id, len(data), data.ljust(8, b"\x00")))

    def sdo_reply(self, data):
        self.send(0x580 + self.id, data.ljust(8, b"\x00"))

    def abort(self, index, sub, code):
        self.sdo = None
        self.sdo_reply(struct.pack("<BHBI", 0x80, index, sub, code))
        print("  SDO abort %04X:%02X %08X" % (index, sub, code))

    def bootup(self):
        self.state = 0x7F
        self.send(0x700 + self.id, b"\x00")
        self.next_hb = time.monotonic() + self.hb_period()
        print("boot-up, pre-operational")

    def hb_period(self):
        ms = struct.unpack("<H", self.od[(0x1017, 0)])[0]
        return ms / 1000.0 if ms else None

    def tick(self):
        p = self.hb_period()
        if p and time.monotonic() >= self.next_hb:
            self.send(0x700 + self.id, bytes([self.state]))
            self.next_hb = time.monotonic() + p

    # -- protocol ---------------------------------------------------------
    def on_frame(self, can_id, data):
        if can_id == 0x000 and len(data) >= 2 and data[1] in (0, self.id):
            cmd = data[0]
            if cmd in NMT_STATES:
                self.state = NMT_STATES[cmd]
                print("NMT -> state 0x%02X" % self.state)
                self.send(0x700 + self.id, bytes([self.state]))   # report change at once
            elif cmd == 0x81:
                print("NMT reset node")
                self.reset_od()
                self.bootup()
            elif cmd == 0x82:
                print("NMT reset communication")
                self.bootup()
        elif can_id == 0x600 + self.id and len(data) == 8:
            if self.state == 0x04:
                return  # stopped nodes do not answer SDO
            self.on_sdo(data)

    def on_sdo(self, d):
        cs = d[0]
        ccs = cs >> 5
        if cs == 0x80:
            self.sdo = None
            return
        if ccs == 2:                       # initiate upload
            index, sub = struct.unpack_from("<HB", d, 1)
            key = (index, sub)
            if key not in self.od:
                return self.abort(index, sub, ABORT_NO_OBJECT)
            val = self.od[key]
            print("  SDO read %04X:%02X -> %s" % (index, sub, val.hex()))
            if len(val) <= 4:
                n = 4 - len(val)
                self.sdo_reply(struct.pack("<BHB", 0x43 | (n << 2), index, sub) + val)
            else:
                self.sdo = {"up": True, "index": index, "sub": sub, "data": val, "pos": 0, "toggle": 0}
                self.sdo_reply(struct.pack("<BHBI", 0x41, index, sub, len(val)))
        elif ccs == 3:                     # upload segment request
            s = self.sdo
            if not s or not s["up"]:
                return self.abort(0, 0, ABORT_CMD)
            t = (cs >> 4) & 1
            if t != s["toggle"]:
                return self.abort(s["index"], s["sub"], ABORT_TOGGLE)
            chunk = s["data"][s["pos"]:s["pos"] + 7]
            s["pos"] += len(chunk)
            last = s["pos"] >= len(s["data"])
            self.sdo_reply(bytes([(t << 4) | ((7 - len(chunk)) << 1) | (1 if last else 0)]) + chunk)
            s["toggle"] ^= 1
            if last:
                self.sdo = None
        elif ccs == 1:                     # initiate download
            index, sub = struct.unpack_from("<HB", d, 1)
            key = (index, sub)
            if key not in self.od:
                return self.abort(index, sub, ABORT_NO_OBJECT)
            if key in self.ro:
                return self.abort(index, sub, ABORT_READ_ONLY)
            if cs & 0x02:                  # expedited
                n = 4 - ((cs >> 2) & 3) if cs & 0x01 else 4
                self.store(index, sub, d[4:4 + n])
                self.sdo_reply(struct.pack("<BHB", 0x60, index, sub))
            else:
                self.sdo = {"up": False, "index": index, "sub": sub, "data": b"", "toggle": 0}
                self.sdo_reply(struct.pack("<BHB", 0x60, index, sub))
        elif ccs == 0:                     # download segment
            s = self.sdo
            if not s or s["up"]:
                return self.abort(0, 0, ABORT_CMD)
            t = (cs >> 4) & 1
            if t != s["toggle"]:
                return self.abort(s["index"], s["sub"], ABORT_TOGGLE)
            n = 7 - ((cs >> 1) & 7)
            s["data"] += d[1:1 + n]
            self.sdo_reply(bytes([0x20 | (t << 4)]))
            s["toggle"] ^= 1
            if cs & 0x01:
                self.store(s["index"], s["sub"], s["data"])
                self.sdo = None
        else:
            self.abort(0, 0, ABORT_CMD)

    def store(self, index, sub, val):
        key = (index, sub)
        old = self.od[key]
        if index != 0x2001 and len(val) != len(old):
            return self.abort(index, sub, ABORT_LENGTH)
        self.od[key] = bytes(val)
        print("  SDO write %04X:%02X <- %s" % (index, sub, bytes(val).hex()))
        if key == (0x2000, 0):
            if val[0] == 0xEE:
                self.emcy_active = True
                self.od[(0x1001, 0)] = b"\x01"
                self.send(0x080 + self.id, struct.pack("<HB5s", 0x5000, 0x01, b"\x12\x34\x00\x00\x00"))
                print("  EMCY 5000 sent")
            elif self.emcy_active and val[0] == 0x00:
                self.emcy_active = False
                self.od[(0x1001, 0)] = b"\x00"
                self.send(0x080 + self.id, struct.pack("<HB5s", 0x0000, 0x00, b""))
                print("  EMCY error reset sent")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("iface")
    ap.add_argument("--node", type=int, default=5)
    ap.add_argument("--seconds", type=float, default=0, help="stop after this many seconds")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    sock.bind((args.iface,))
    sock.settimeout(0.05)
    node = Node(sock, args.node)
    node.bootup()
    end = time.monotonic() + args.seconds if args.seconds else None
    while end is None or time.monotonic() < end:
        try:
            raw = sock.recv(16)
            can_id, length, data = struct.unpack(FMT, raw)
            if not can_id & 0xE0000000:      # standard data frames only
                node.on_frame(can_id & 0x7FF, data[:length])
        except socket.timeout:
            pass
        node.tick()


if __name__ == "__main__":
    main()
