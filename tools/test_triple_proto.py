# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
"""Tests for triple_proto.py: round trips plus the byte sequences captured from
firmware 1.41 (the same vectors as tests/codec/codec_test.c).

    python3 tools/test_triple_proto.py
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import triple_proto as tp  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("  FAIL:", what)


def hx(s):
    return bytes.fromhex(s)


def roundtrip(f, special=False):
    wire = tp.encode_frame(f)
    check(wire[1] == len(wire), "raw length byte %r" % f)
    check(wire[0] == tp.FIRST and wire[-1] == tp.LAST, "framing %r" % f)
    for i in range(2, len(wire) - 1):
        if wire[i] == tp.LAST:
            check(wire[i - 1] == tp.ESC, "bare end byte inside %r" % f)
    kind, g = tp.decode(wire)
    check(kind == "frame", "decode kind %r" % f)
    check((g.port, g.id, g.eff, g.rtr, g.fd, g.brs) == (f.port, f.id, f.eff, f.rtr, f.fd, f.brs), "fields %r -> %r" % (f, g))
    check(g.data == (b"" if f.rtr else f.data), "data %r" % f)
    check(g.ts is None, "no timestamp on TX layout")
    return wire


print("--- round trips")
roundtrip(tp.Frame(port=0, id=0x123, data=bytes([0x0F, 0xEF, 0x1F, 3, 0x0F, 0xEF, 0x1F, 7])))
roundtrip(tp.Frame(port=1, id=0x1ABCDEF0, eff=True))
roundtrip(tp.Frame(port=0, id=0x7FF, rtr=True, data=b"\x00" * 4))
w = roundtrip(tp.Frame(port=0, id=0x123, data=b"\x11\x22\x33\x44\x55\x66"))
check(len(w) == 15 and w[1] == 0x0F, "15-byte message keeps raw 0x0F length")
roundtrip(tp.Frame(port=2, id=0x100, fd=True, brs=True, data=bytes(range(12))))
roundtrip(tp.Frame(port=2, id=0x1FFFFFFF, eff=True, fd=True, data=bytes([0x0F, 0xEF, 0x1F]) * 4))
roundtrip(tp.Frame(port=2, id=0x100, fd=True, data=bytes(range(64))))

print("--- captured from firmware 1.41")
kind, f = tp.decode(hx("0f 15 b0 00 00 01 23 08 00 29 35 82 11 22 33 44 55 66 77 88 ef"))
check(kind == "frame" and f.port == 0 and not f.eff and f.id == 0x123 and f.ts == 0x00293582 and f.data == hx("1122334455667788"), "ts std frame")
kind, f = tp.decode(hx("0f 12 b0 1a bc de f0 84 00 2d d3 34 de ad be 1f ef ef"))
check(kind == "frame" and f.eff and f.id == 0x1ABCDEF0 and f.data == hx("deadbeef") and f.ts == 0x002DD334, "ts ext frame with escape")
kind, f = tp.decode(hx("0f 19 b2 00 00 01 00 39 00 37 0d 15 00 01 02 03 04 05 06 07 08 09 0a 0b ef"))
check(kind == "frame" and f.port == 2 and f.fd and f.brs and not f.esi and len(f.data) == 12, "ts fd brs frame")
kind, f = tp.decode(hx("0f 0f a1 00 00 01 23 06 11 22 33 44 55 66 ef"))
check(kind == "frame" and f.port == 1 and len(f.data) == 6 and f.ts is None, "15-byte rx frame")
kind, f = tp.decode(hx("0f 0a b3 00 00 00 10 20 00 00 00 01 ef"))
check(kind == "frame" and f.port == 2 and f.esi and f.fd and f.data == b"", "esi variant")
kind, v = tp.decode(hx("0f 08 90 31 2e 34 31 ef"))
check(kind == "fw" and v == "1.41", "firmware version")
kind, st = tp.decode(hx("0f 2a 8a 05 80 08 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 20 00 01 00 00 00 00 00 00 00 00 ef"))
check(kind == "status" and st.tec[0] == 0x80 and st.rec[0] == 8 and st.state[0] == 1 and st.tec[1] == 0, "status port 1 passive")
kind, st = tp.decode(hx("0f 2a 8a 05 00 00 00 80 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 20 00 00 01 00 00 00 00 00 00 00 ef"))
check(kind == "status" and st.rec[1] == 0x80 and st.state[1] == 1, "status port 2 passive")
check(tp.decode(hx("0f 05 87 01 ef"))[0] == "unknown", "marker reported as unknown")
try:
    tp.decode(hx("0f 09 a0 00 00 01 23 08 ef"))
    check(False, "truncated frame must raise")
except ValueError:
    pass

print("--- commands, compared with bytes sent to firmware 1.41")
check(tp.settings_can(1, 250) == hx("0f 08 88 01 00 fa 00 ef"), "settings can1 250k")
check(tp.settings_fd(1256700, iso=False) == hx("0f 0c 88 03 00 13 2c fc 00 00 00 ef"), "settings fd 125k/6.7M")
check(tp.timestamp_mode(True) == hx("0f 05 8b 01 ef"), "timestamp on")
check(tp.buffer_mode(1) == hx("0f 08 8d 00 00 00 01 ef"), "buffer mode")
check(tp.fw_version_req() == hx("0f 04 90 ef"), "fw request")
check(tp.bittiming_fd(1, 62, 17, 17, 1, 15, 4, 4, 0, 0, 2) ==
      hx("0f 1f 89 03 00 01 00 3e 00 11 00 11 00 01 00 1f 0f 00 04 00 04 00 00 00 00 00 02 00 01 00 ef"), "raw bit timing (same vector as the C test)")

print("--- CAN FD padding to the DLC length")
w = tp.encode_frame(tp.Frame(port=2, id=0x345, fd=True, data=bytes(range(0xA0, 0xAA))))
kind, g = tp.decode(w)
check(kind == "frame" and len(g.data) == 12 and g.data[:10] == bytes(range(0xA0, 0xAA)) and g.data[10:] == bytes(2),
      "10 bytes sent as 12 with zero padding")

print("--- CAN FD raw bit timing (verified on the bus)")
regs = tp.fd_bittiming_from_tq(1, 63, 16, 16, 1, 15, 4, 4)
check(regs == (0, 62, 15, 15, 0, 14, 3, 3, 16, 0, 2), "500k/2M registers %r" % (regs,))
check(tp.fd_bittiming_from_tq(1, 63, 16, 16, 1, 6, 1, 1)[8:] == (7, 0, 2), "500k/5M delay compensation")
check(tp.fd_bittiming_from_tq(2, 63, 16, 0, 4, 15, 4, 0)[8:] == (0, 0, 0), "no compensation above prescaler 2")
check(tp.fd_bittiming_from_tq(1, 63, 16, 16, 1, 33, 4, 4) is None, "data tseg1 out of range")
check(tp.bittiming_fd(0, 62, 15, 15, 0, 14, 3, 3, 15, 0, 2) ==
      hx("0f 21 89 03 00 00 00 3e 00 1f 0f 00 1f 0f 00 00 00 0e 00 03 00 03 00 1f 0f 00 00 00 02 00 01 00 ef"), "bus test 500k/2M bytes")
check(tp.fd_timing_for(800000, 0.8, False) == (1, 39, 10, 10), "800k nominal at 80 %")
check(tp.fd_timing_for(4000000, 0.8, True) == (1, 7, 2, 2), "4M data at 80 %")
check(tp.fd_timing_for(500000, 0.8, True) == (2, 31, 8, 8), "500k data needs prescaler 2")
check(tp.fd_timing_for(20000000, 0.8, True) is None, "20M unreachable")
check(tp.bittiming_fd(*tp.fd_bittiming_from_tq(1, 39, 10, 10, 1, 7, 2, 2))[3:5] == bytes([3, 0]), "800k/4M encodes")

print("--- bitrate tables")
check(tp.can_speed_kbit(250000) == 250 and tp.can_speed_kbit(33333) == 33 and tp.can_speed_kbit(300000) == 0, "can speeds")
check(tp.fd_speed_code(125000, 6700000) == 1256700 and tp.fd_speed_code(125000, 250000) == 125250, "fd codes 1")
check(tp.fd_speed_code(1000000, 1500000) == 10001500 and tp.fd_speed_code(125000, 10000000) == 1259999, "fd codes 2")
check(tp.fd_speed_code(500000, 250000) == 0 and tp.fd_speed_code(300000, 1000000) == 0, "fd codes invalid")
check(tp.fd_default_data_bitrate(125000) == 250000, "fd default data")

print("--- stream parser")
p = tp.Parser()
stream = b"\x00\x00" + hx("0f 08 90 31 2e 34 31 ef") + hx("0f 0f a1 00 00 01 23 06 11 22 33 44 55 66 ef")[:9]
msgs = p.feed(stream)
check(len(msgs) == 1 and tp.decode(msgs[0]) == ("fw", "1.41"), "first message, leading garbage skipped")
msgs = p.feed(hx("0f 0f a1 00 00 01 23 06 11 22 33 44 55 66 ef")[9:] + hx("0f 12 b0 1a bc de f0 84 00 2d d3 34 de ad be 1f ef ef"))
check(len(msgs) == 2, "split across feeds and escaped end byte")
check(p.dropped == 2, "dropped count")

print("PYTHON CODEC TEST PASSED" if not fails else "PYTHON CODEC TEST FAILED (%d failures)" % fails)
sys.exit(1 if fails else 0)
