#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
"""Talk to a USB2CAN Triple directly over its serial port, no kernel driver.
Works on Linux, Windows and macOS (needs pyserial: pip install pyserial).

Examples:
    triple_dump.py /dev/ttyACM0                       # 250k / 500k / FD 125k+6.7M, print frames
    triple_dump.py COM7 --can1 500000 --listen-only   # Windows, CAN1 at 500 kbit/s, listen only
    triple_dump.py /dev/ttyACM0 --send 1 123#11223344 # send one frame on CAN1 and keep dumping
    triple_dump.py /dev/ttyACM0 --status              # also print raw status messages
    triple_dump.py /dev/ttyACM0 --log frames.log      # candump-like log file
    triple_dump.py COM7 --can3-timing 0:62:15:15:0:14:3:3:15:0:2   # CAN3 raw bit timing

The Linux kernel driver must NOT be attached to the port at the same time.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import triple_proto as tp  # noqa: E402

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")


def parse_frame_arg(port: int, text: str) -> tp.Frame:
    """cansend syntax: 123#1122, 1ABCDEF0#DEADBEEF, 123#R, 100##1.0011 (FD, flags digit)."""
    ident, _, rest = text.partition("#")
    f = tp.Frame(port=port - 1, id=int(ident, 16), eff=len(ident) > 3)
    if rest.startswith("#"):
        f.fd = True
        flags, data = int(rest[1], 16), rest[2:]
        f.brs = bool(flags & 1)
        f.esi = bool(flags & 2)
        f.data = bytes.fromhex(data.replace(".", ""))
    elif rest.upper().startswith("R"):
        f.rtr = True
        f.data = b"\x00" * int(rest[1:] or "0")
    else:
        f.data = bytes.fromhex(rest.replace(".", ""))
    return f


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("port", help="/dev/ttyACMx or COMx")
    ap.add_argument("--can1", type=int, default=250000, help="CAN1 bitrate, bit/s (default 250000)")
    ap.add_argument("--can2", type=int, default=500000, help="CAN2 bitrate, bit/s (default 500000)")
    ap.add_argument("--can3", default="125000/6700000", help="CAN3 nominal/data bitrate (default 125000/6700000)")
    ap.add_argument("--listen-only", action="store_true", help="all ports listen only")
    ap.add_argument("--non-iso", action="store_true", help="CAN3 non-ISO CAN FD")
    ap.add_argument("--can3-timing", metavar="NBRP:NTSEG1:NTSEG2:NSJW:DBRP:DTSEG1:DTSEG2:DSJW:TDCO:TDCV:TDCMOD",
                    help="CAN3 raw MCP2517FD bit timing (command 0x89) instead of --can3; see doc/protocol.md 4.2")
    ap.add_argument("--no-timestamp", action="store_true", help="do not ask the adapter for timestamps")
    ap.add_argument("--status", action="store_true", help="print status messages (error counters) when they change")
    ap.add_argument("--raw", action="store_true", help="hex dump every message")
    ap.add_argument("--send", nargs=2, metavar=("PORT", "FRAME"), action="append", default=[],
                    help="send a frame on PORT (1..3), cansend syntax, may repeat")
    ap.add_argument("--log", help="append frames to this file, one per line")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this many seconds (default: run until Ctrl-C)")
    args = ap.parse_args()

    k1, k2 = tp.can_speed_kbit(args.can1), tp.can_speed_kbit(args.can2)
    nominal, _, data = args.can3.partition("/")
    nominal = int(nominal)
    data = int(data) if data else tp.fd_default_data_bitrate(nominal)
    code = tp.fd_speed_code(nominal, data)
    raw = None
    if not code and not args.can3_timing:
        # not in the firmware table: raw controller timing at 80 % sample
        # points, which is what the table uses (doc/protocol.md 4.2)
        n, d = tp.fd_timing_for(nominal, 0.8, False), tp.fd_timing_for(data, 0.8, True)
        raw = tp.fd_bittiming_from_tq(*n, *d) if n and d else None
    if not k1 or not k2 or not (code or raw or args.can3_timing):
        sys.exit("unsupported bitrate; see doc/protocol.md section 4.1")

    ser = serial.Serial(args.port, 115200, timeout=0.05)
    if args.can3_timing:
        t = [int(v, 0) for v in args.can3_timing.split(":")]
        if len(t) != 11:
            sys.exit("--can3-timing needs eleven values")
        can3 = tp.bittiming_fd(*t, listen_only=args.listen_only, iso=not args.non_iso)
    elif raw:
        can3 = tp.bittiming_fd(*raw, listen_only=args.listen_only, iso=not args.non_iso)
        args.can3_timing = ":".join(str(v) for v in raw)
    else:
        can3 = tp.settings_fd(code, args.listen_only, not args.non_iso)
    cmds = [tp.buffer_mode(1), tp.timestamp_mode(not args.no_timestamp),
            tp.settings_can(1, k1, args.listen_only), tp.settings_can(2, k2, args.listen_only),
            can3, tp.fw_version_req()]
    for c in cmds:
        ser.write(c)
        time.sleep(0.02)
    for port, text in args.send:
        wire = tp.encode_frame(parse_frame_arg(int(port), text))
        if len(wire) > tp.FW_MAX_WIRE_LEN:
            sys.exit("frame too long for the adapter (%d bytes on the wire, limit %d)" % (len(wire), tp.FW_MAX_WIRE_LEN))
        ser.write(wire)

    print("CAN1 %d, CAN2 %d, CAN3 FD %s%s" % (args.can1, args.can2,
                                             "timing " + args.can3_timing if args.can3_timing else "%d/%d" % (nominal, data),
                                             ", listen only" if args.listen_only else ""))
    log = open(args.log, "a") if args.log else None
    parser = tp.Parser()
    last_status = None
    t_end = time.time() + args.seconds if args.seconds else None
    try:
        while t_end is None or time.time() < t_end:
            chunk = ser.read(4096)
            if not chunk:
                continue
            for wire in parser.feed(chunk):
                if args.raw:
                    print("   " + wire.hex(" "))
                try:
                    kind, msg = tp.decode(wire)
                except ValueError as e:
                    print("?? %s: %s" % (e, wire.hex(" ")))
                    continue
                if kind == "frame":
                    line = str(msg)
                    print(line)
                    if log:
                        log.write(line + "\n")
                elif kind == "fw":
                    print("firmware %s" % msg)
                elif kind == "status":
                    if args.status and msg.raw != last_status:
                        last_status = msg.raw
                        print("status tec %s rec %s state %s raw %s" % (msg.tec, msg.rec, msg.state, msg.raw.hex(" ")))
                else:
                    print("unknown command 0x%02X: %s" % (msg, wire.hex(" ")))
    except KeyboardInterrupt:
        pass
    finally:
        if log:
            log.close()
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
