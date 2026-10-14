"""USB2CAN Triple wire protocol, Python reference implementation.

Mirrors driver/triple_proto.c. See doc/protocol.md for the specification.
No dependencies beyond the standard library. Python 3.8+.

    from triple_proto import *
    wire = encode_frame(Frame(port=0, id=0x123, data=b"\\x11\\x22"))
    kind, msg = decode(wire)        # ("frame", Frame(...))

A stream from the adapter is split into messages with Parser.feed().
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Tuple

FIRST = 0x0F
LAST = 0xEF
ESC = 0x1F

CMD_TX_CAN_1 = 0xA0          # also RX without timestamp, ports 1..3 = A0..A2, A3 = port 3 with ESI
CMD_RX_TS_CAN_1 = 0xB0       # RX with timestamp, B0..B3
CMD_SETTINGS = 0x88
CMD_BITTIMING = 0x89
CMD_STATUS = 0x8A
CMD_TIMESTAMP = 0x8B
CMD_BUFFER_MODE = 0x8D
CMD_FW_VER = 0x90
CMD_MARKER = 0x87

FLAG_EFF = 0x80
FLAG_RTR = 0x40
FLAG_FD = 0x20
FLAG_BRS = 0x10
FLAG_DLC = 0x0F

MTU = 160                    # longest message either side can produce is 143
FW_MAX_WIRE_LEN = 100        # largest message verified to be accepted by firmware 1.41
STATUS_LEN = 38

_DLC2LEN = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64]


def dlc2len(dlc: int) -> int:
    return _DLC2LEN[dlc & FLAG_DLC]


def len2dlc(length: int) -> int:
    if length <= 8:
        return length
    for code in range(9, 16):
        if length <= _DLC2LEN[code]:
            return code
    raise ValueError("length > 64")


@dataclass
class Frame:
    port: int = 0                   # 0..2 = CAN1..CAN3
    id: int = 0                     # identifier without flag bits
    data: bytes = b""
    eff: bool = False               # extended (29-bit) identifier
    rtr: bool = False
    fd: bool = False
    brs: bool = False
    esi: bool = False
    ts: Optional[int] = None        # adapter microseconds (32 bit), receive only

    def __str__(self) -> str:
        ident = "%08X" % self.id if self.eff else "%03X" % self.id
        flags = ("R" if self.rtr else "") + ("F" if self.fd else "") + ("B" if self.brs else "") + ("E" if self.esi else "")
        ts = "" if self.ts is None else "%10.6f " % (self.ts / 1e6)
        return "%scan%d %s%s [%d] %s" % (ts, self.port + 1, ident, (" " + flags) if flags else "", len(self.data), self.data.hex(" ").upper())


@dataclass
class Status:
    tec: List[int] = field(default_factory=lambda: [0, 0, 0])
    rec: List[int] = field(default_factory=lambda: [0, 0, 0])
    state: List[int] = field(default_factory=lambda: [0, 0, 0])   # 0 active, 1 passive seen
    raw: bytes = b""


# ---- escaping ---------------------------------------------------------

def escape(payload: bytes) -> bytes:
    out = bytearray()
    for b in payload:
        if b in (FIRST, LAST, ESC):
            out.append(ESC)
        out.append(b)
    return bytes(out)


def unescape(wire: bytes) -> bytes:
    """Unescape a complete message. Bytes 0 and 1 (start, raw length) are kept as they are."""
    out = bytearray(wire[:2])
    esc = False
    for b in wire[2:]:
        if b == ESC and not esc:
            esc = True
            continue
        out.append(b)
        esc = False
    return bytes(out)


def build(cmd: int, payload: bytes = b"") -> bytes:
    body = escape(bytes([cmd]) + payload)
    n = 2 + len(body) + 1
    if n > 255:
        raise ValueError("message too long")
    return bytes([FIRST, n]) + body + bytes([LAST])


# ---- frames ------------------------------------------------------------

def encode_frame(f: Frame) -> bytes:
    if not 0 <= f.port <= 2:
        raise ValueError("port must be 0..2")
    if len(f.data) > (64 if f.fd else 8):
        raise ValueError("too much data")
    flags = len2dlc(len(f.data))
    if f.eff:
        flags |= FLAG_EFF
    if f.rtr:
        flags |= FLAG_RTR
    if f.fd:
        flags |= FLAG_FD
        if f.brs:
            flags |= FLAG_BRS
    payload = f.id.to_bytes(4, "big") + bytes([flags]) + (b"" if f.rtr else f.data)
    return build(CMD_TX_CAN_1 + f.port, payload)


def decode(wire: bytes) -> Tuple[str, object]:
    """Decode one complete message. Returns (kind, value):
    ("frame", Frame), ("status", Status), ("fw", str), ("unknown", cmd) or raises ValueError."""
    if len(wire) < 4 or wire[0] != FIRST or wire[-1] != LAST:
        raise ValueError("not a message")
    buf = unescape(wire)
    cmd = buf[2]
    p = buf[3:-1]

    if cmd == CMD_STATUS:
        if len(p) < STATUS_LEN:
            raise ValueError("short status")
        st = Status(raw=bytes(p[:STATUS_LEN]))
        st.tec = [p[1], p[3], p[5]]
        st.rec = [p[2], p[4], p[6]]
        st.state = [p[29], p[30], p[31]]
        return "status", st

    if cmd == CMD_FW_VER:
        return "fw", p.decode("ascii", "replace")

    if CMD_TX_CAN_1 <= cmd <= CMD_TX_CAN_1 + 3:
        idx, has_ts = cmd - CMD_TX_CAN_1, False
    elif CMD_RX_TS_CAN_1 <= cmd <= CMD_RX_TS_CAN_1 + 3:
        idx, has_ts = cmd - CMD_RX_TS_CAN_1, True
    else:
        return "unknown", cmd

    need = 5 + (4 if has_ts else 0)
    if len(p) < need:
        raise ValueError("short frame")
    f = Frame(port=min(idx, 2), esi=(idx == 3))
    f.id = int.from_bytes(p[0:4], "big")
    flags = p[4]
    pos = 5
    if has_ts:
        f.ts = int.from_bytes(p[5:9], "big")
        pos = 9
    f.eff = bool(flags & FLAG_EFF)
    f.rtr = bool(flags & FLAG_RTR)
    f.fd = bool(flags & FLAG_FD)
    f.brs = f.fd and bool(flags & FLAG_BRS)
    length = dlc2len(flags)
    if not f.fd and length > 8:
        raise ValueError("bad dlc")
    if not f.rtr:
        if len(p) - pos < length:
            raise ValueError("truncated data")
        f.data = bytes(p[pos:pos + length])
    return "frame", f


class Parser:
    """Splits the byte stream from the adapter into complete messages."""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.esc = False
        self.dropped = 0

    def feed(self, data: bytes) -> List[bytes]:
        out: List[bytes] = []
        for b in data:
            if not self.buf:
                if b == FIRST:
                    self.buf.append(b)
                    self.esc = False
                else:
                    self.dropped += 1
                continue
            if len(self.buf) == 1:          # raw length byte
                self.buf.append(b)
                continue
            if len(self.buf) >= MTU:        # out of sync, resync on the next start byte
                self.dropped += len(self.buf)
                self.buf.clear()
                continue
            self.buf.append(b)
            if self.esc:
                self.esc = False
                continue
            if b == ESC:
                self.esc = True
            elif b == LAST:
                out.append(bytes(self.buf))
                self.buf.clear()
        return out


# ---- commands ----------------------------------------------------------

def settings_can(port: int, kbit: int, listen_only: bool = False) -> bytes:
    """port 1 or 2, bitrate code in kbit/s (see can_speed_kbit)."""
    return build(CMD_SETTINGS, bytes([port]) + kbit.to_bytes(2, "big") + bytes([1 if listen_only else 0]))


def settings_fd(code: int, listen_only: bool = False, iso: bool = True, esi: bool = False) -> bytes:
    """port 3, speed code from fd_speed_code()."""
    return build(CMD_SETTINGS, bytes([3]) + code.to_bytes(4, "big") + bytes([int(listen_only), int(iso), int(esi)]))


def bittiming_fd(nbrp: int, ntseg1: int, ntseg2: int, nsjw: int, dbrp: int, dtseg1: int, dtseg2: int, dsjw: int,
                 tdco: int, tdcv: int, tdcmod: int, listen_only: bool = False, iso: bool = True, esi: bool = False) -> bytes:
    """port 3 raw MCP2517FD bit timing (unverified against firmware)."""
    payload = bytes([3])
    for v in (nbrp, ntseg1, ntseg2, nsjw, dbrp, dtseg1, dtseg2, dsjw, tdco, tdcv, tdcmod):
        payload += v.to_bytes(2, "big")
    payload += bytes([int(listen_only), int(iso), int(esi)])
    return build(CMD_BITTIMING, payload)


def timestamp_mode(on: bool) -> bytes:
    return build(CMD_TIMESTAMP, bytes([1 if on else 0]))


def buffer_mode(mode: int = 1) -> bytes:
    return build(CMD_BUFFER_MODE, mode.to_bytes(4, "big"))


def fw_version_req() -> bytes:
    return build(CMD_FW_VER)


# ---- bitrate tables ----------------------------------------------------

CAN_SPEEDS = {10000: 10, 20000: 20, 33333: 33, 50000: 50, 62500: 62, 83333: 83, 100000: 100,
              125000: 125, 250000: 250, 500000: 500, 1000000: 1000}
FD_NOMINAL = (125000, 250000, 500000, 1000000)
FD_DATA = {250000: 250, 500000: 500, 833333: 833, 1000000: 1000, 1500000: 1500, 2000000: 2000, 3000000: 3000,
           4000000: 4000, 5000000: 5000, 6700000: 6700, 8000000: 8000, 10000000: 9999}


def can_speed_kbit(bitrate: int) -> int:
    """Firmware code for a CAN 2.0 bitrate in bit/s, 0 if unsupported."""
    return CAN_SPEEDS.get(bitrate, 0)


def fd_speed_code(nominal: int, data: int) -> int:
    """Firmware code for a CAN FD nominal/data bitrate pair in bit/s, 0 if unsupported."""
    if nominal not in FD_NOMINAL or data not in FD_DATA or data <= nominal:
        return 0
    nom_k, data_code = nominal // 1000, FD_DATA[data]
    return nom_k * 10000 + data_code if data_code >= 1000 else nom_k * 1000 + data_code


def fd_default_data_bitrate(nominal: int) -> int:
    for d in sorted(FD_DATA):
        if d > nominal:
            return d
    return 0
