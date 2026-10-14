# USB2CAN Triple host protocol

Reference for the byte protocol between a host and the CANLAB USB2CAN
Triple adapter. Written from the Linux driver (`driver/triple_proto.[ch]`)
and from captures taken against **firmware 1.41** on 2026-10-14 with two
adapters wired back to back. Everything marked *verified* was observed on
that hardware; everything marked *unverified* comes from older driver code
or is a guess and is labelled as such.

The same codec is implemented in `driver/triple_proto.c` (C, used by the
kernel module and by `tests/codec`) and `tools/triple_proto.py` (Python,
used by `tools/triple_dump.py`). Both are checked against the captured
byte sequences listed at the end of this document.

## 1. Transport

| Item | Value |
|---|---|
| USB | full speed, CDC ACM (`cdc_acm` on Linux, `usbser.sys` on Windows) |
| VID:PID | `04d8:f0d7` (Microchip) |
| Strings | manufacturer "CANLAB s.r.o. www.canlab.cz", product "USB2CAN triple", serial number e.g. `012345678A` |
| Serial settings | irrelevant for CDC ACM; the Linux driver does not set a baud rate, the old daemon used 115200 8N1 raw |
| Direction | full duplex, byte stream, no ACK or sequence numbers |

The adapter has three CAN connectors: CAN1 and CAN2 are classic CAN 2.0
(high speed, ISO 11898-2), CAN3 is CAN FD (MCP2517FD). Port numbers in the
protocol are 1, 2, 3; the Linux driver uses 0, 1, 2 internally.

## 2. Message framing

Every message in both directions:

```
offset  size  content
0       1     0x0F   start byte
1       1     length: total number of bytes of this message on the wire,
              escape bytes included, start and end byte included. RAW,
              never escaped (verified: a 15-byte message carries 0x0F here)
2       1     command (see sections 4 and 5)
3..     n     payload, escaped
last    1     0xEF   end byte
```

### 2.1 Escaping

Inside the payload (offset 2 onward, the command byte included) the three
special values are prefixed by the escape byte:

| Value | Meaning when unescaped |
|---|---|
| `0x0F` | start byte |
| `0xEF` | end byte |
| `0x1F` | escape byte |

So the payload byte `0xEF` is sent as `1F EF`, `0x1F` as `1F 1F`, `0x0F`
as `1F 0F`. The command byte is in principle subject to the same rule but
no command value is special.

The length byte is **not** escaped. The firmware reads it as a plain byte
count before it unescapes anything (*verified*: sending `0F 1F 10 ...` for
a 16-byte message made the firmware wait for 31 bytes and swallow the
following message as well).

A receiver therefore works like this: wait for `0x0F`, take the next byte
raw as the length, then collect bytes until an unescaped `0xEF`, resolving
escapes from offset 2 on. The length byte can be used as a cross check
only.

### 2.2 Size limits

| Limit | Value | Source |
|---|---|---|
| Longest message either side can build | 143 bytes (FD frame, 64 data bytes all escaped: 1 + 1 + 2 + 8 + 2 + 128 + 1) | arithmetic |
| Longest message the firmware is known to accept from the host | 100 bytes | *verified* |
| Host-bound message that locked the adapter up | 137 bytes | *verified* |

A 137-byte message (FD frame, all data bytes escaped) stopped the adapter
from transmitting anything on any port until it was power cycled; status
messages kept coming, a USB reset did not help. The real limit lies
between 101 and 136 bytes and should be taken from the firmware source.
The Linux driver drops frames longer than `max_wire_len` (default 100).
100 bytes covers an FD frame with 64 data bytes of which up to 27 need
escaping.

## 3. CAN frame payload

Used by the transmit commands (host to adapter) and the receive commands
(adapter to host). The firmware uses the same layout in both directions
(*verified*).

```
offset  size  content
0       4     CAN identifier, big endian, 11 or 29 bits, no flag bits
4       1     flags | DLC code (see below)
5       4     timestamp, big endian, microseconds, 32 bit, wraps.
              Present only in the receive commands 0xB0..0xB3
5/9     n     data bytes, n = length from the DLC code (0 for RTR frames)
```

Flags byte:

| Bit | Meaning |
|---|---|
| `0x80` | extended (29-bit) identifier. Same polarity in both directions (*verified*; driver v1.3 had it inverted on receive) |
| `0x40` | RTR (classic CAN). The old driver also set it for FD frames; FD has no RTR |
| `0x20` | CAN FD frame |
| `0x10` | bit rate switch (FD only) |
| `0x0F` | DLC code |

DLC code to length, CAN FD encoding (classic CAN only uses 0..8):

| Code | 0..8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|
| Bytes | 0..8 | 12 | 16 | 20 | 24 | 32 | 48 | 64 |

Toward the adapter, send all bytes of the DLC length. A frame with fewer
data bytes than its DLC code stands for (say 10 bytes with code 9, 12
bytes) goes out on the bus with the gap filled from the firmware's buffer,
i.e. with bytes of an earlier frame (*verified*: a 10-byte frame sent after
a 20-byte one carried that frame's bytes 11 and 12). The codecs pad with
zeros since 2026-10-14; the Linux driver was not affected because the
kernel already rounds CAN FD lengths up to a DLC step.

Timestamp: 1 µs resolution, 32 bit, big endian, counts from an unknown
origin (it started near 2.7 s after the timestamp mode was switched on in
the capture) and wraps every 71.6 minutes. Consecutive frames sent 100 ms
apart showed deltas of 102 ms including host scheduling (*verified*).

## 4. Host to adapter

| Command | Name | Payload |
|---|---|---|
| `0xA0` | TX on CAN1 | CAN frame payload (section 3, no timestamp) |
| `0xA1` | TX on CAN2 | CAN frame payload |
| `0xA2` | TX on CAN3 | CAN frame payload, FD flags allowed |
| `0x88` | SETTINGS | port settings, two layouts, see 4.1 |
| `0x89` | BITTIMING | raw MCP2517FD bit timing for CAN3, see 4.2 (*verified*) |
| `0x8B` | TIMESTAMP | 1 byte: `0` off, `1` on. Selects 0xA0.. or 0xB0.. receive commands |
| `0x8D` | BUFFER_MODE | 4 bytes big endian, the Linux driver sends `1` (*purpose not documented; always sent since v1.x*) |
| `0x90` | FW_VERSION | no payload, answered with 0x90 (section 5) |
| `0x81` | TX (legacy) | CAN frame payload, port in a trailing byte; from driver v1.x, not used (*unverified*) |
| `0x87` | MARKER | trigger input related; not decoded |
| `0x91`, `0x92` | SPEED_DOWN / SPEED_UP | from driver v1.x headers, never used (*unverified*) |

Commands are accepted back to back; the Linux driver waits only for the
tty to take each one and inserts 20 ms between the three attach commands.
The old daemon slept 2 s between commands, which is not necessary.

### 4.1 SETTINGS (0x88)

CAN1 and CAN2:

```
offset  size  content
0       1     port, 1 or 2
1       2     bitrate in kbit/s, big endian: 10 20 33 50 62 83 100 125 250 500 1000
3       1     listen only: 0 or 1
```

Example (*verified*): CAN1 at 250 kbit/s: `0F 08 88 01 00 FA 00 EF`.

CAN3 (always CAN FD in the firmware):

```
offset  size  content
0       1     port, 3
1       4     speed code, big endian, see below
5       1     listen only
6       1     ISO CRC: 1 ISO 11898-1:2015, 0 non-ISO (Bosch)
7       1     ESI: transmit frames with the error state indicator set (0 normally)
```

Example (*verified*): 125 kbit/s nominal, 6.7 Mbit/s data, ISO:
`0F 0C 88 03 00 13 2C FC 00 00 00 EF` (0x00132CFC = 1256700).

Speed code: the decimal concatenation of nominal and data bitrate in
kbit/s, with 10 Mbit/s written as 9999:

| Nominal | Data (kbit/s) | Code |
|---|---|---|
| 125 | 250 500 833 | 125250 125500 125833 |
| 125 | 1000 1500 2000 3000 4000 5000 6700 8000 10000 | 1251000 1251500 1252000 1253000 1254000 1255000 1256700 1258000 1259999 |
| 250 | 500 833 | 250500 250833 |
| 250 | 1000 .. 10000 | 2501000 .. 2509999 |
| 500 | 833 | 500833 |
| 500 | 1000 .. 10000 | 5001000 .. 5009999 |
| 1000 | 1500 .. 10000 | 10001500 .. 10009999 |

Rule: data rate must be above the nominal rate; both from the lists.
Formula: `code = nominal_k * 10000 + data_k` when `data_k >= 1000`, else
`nominal_k * 1000 + data_k`.

### 4.2 BITTIMING (0x89), CAN3 raw bit timing (*verified*)

Sets CAN3 from raw MCP2517FD bit timing instead of the speed code table of
4.1. Layout from the v1.x daemon (`tripled -c`); all values 16 bit big
endian unless noted.

```
offset  content
0       port, 3
1       NBRP     nominal bit rate prescaler      \
3       NTSEG1   nominal time segment 1           |
5       NTSEG2   nominal time segment 2           |  register values:
7       NSJW     nominal sync jump width          |  the count minus one
9       DBRP     data bit rate prescaler          |  (0 = divide by 1,
11      DTSEG1   data time segment 1              |   NTSEG1 61 = 62 TQ)
13      DTSEG2   data time segment 2              |
15      DSJW     data sync jump width            /
17      TDCO     transmitter delay compensation offset, clock cycles, signed
19      TDCV     transmitter delay compensation value (0 with automatic mode)
21      TDCMOD   0 off, 1 manual, 2 automatic
23      listen only (1 byte)
24      ISO CRC  (1 byte), as in 4.1
25      ESI      (1 byte), as in 4.1
```

TSEG1 is propagation plus phase segment 1 (`prop_seg + phase_seg1` in
Linux terms). The controller runs at 40 MHz: one time quantum is
`(BRP + 1) * 25 ns`, a bit is `1 + (TSEG1 + 1) + (TSEG2 + 1)` quanta.
Example, 500 kbit/s and 2 Mbit/s at 80 % sample points with automatic
delay compensation: `0:62:15:15 / 0:14:3:3`, TDCO 16, TDCMOD 2.

Verified 2026-10-14 between two adapters wired back to back, one set with
this command, the other with the speed codes of 4.1, CAN FD frames with
and without BRS in both directions:

| Raw timing | Other side | Result |
|---|---|---|
| 500k/2M (`0:62:15:15:0:14:3:3`, TDC auto) | 500k/2M | works |
| same values with NBRP and DBRP 1 (250k/1M) | 500k/2M | nothing gets through, TX errors |
| same values with NBRP and DBRP 1 | 250k/1M | works |
| 500k nominal, DBRP 1 (1M data) | 500k/2M | classic frames pass, BRS frames fail |
| 500k/5M (`0:5:0:0` data, TDCO 7, auto) | 500k/5M | works |
| 800k/4M (`0:38:9:9:0:6:1:1`, TDCO 8, auto) | same raw timing | works (not possible with 4.1) |

So the values are register encoded (a "count" reading would make the first
row an invalid prescaler 0), the clock is 40 MHz (20 MHz would have given
250k/1M in the first row), and the data phase fields are applied
separately. The command needs no trailing byte (the v1.x daemon wrote one
garbage byte after the end byte; not needed).

Sample point: the speed code table uses about 80 % in both phases. Kernel
computed timing with the Linux defaults (87.5 % nominal below 800 kbit/s,
75 % data) lost BRS frames coming from an adapter set by speed code at
500k/2M and 250k/1M, and the other way round at 125k/6.7M; with 80 % /
80 % all four tested pairs (also 1M/5M) worked both ways. In CAN FD the
bit rate switches at the nominal sample point, so nodes need matching
nominal sample points. The Linux driver uses this command with
`fd_bittiming=1`.

## 5. Adapter to host

| Command | Name | Payload |
|---|---|---|
| `0xA0` `0xA1` `0xA2` | RX on CAN1 / CAN2 / CAN3 | CAN frame payload without timestamp (timestamp mode off) |
| `0xA3` | RX on CAN3 with ESI | as 0xA2, the received FD frame had the error state indicator set |
| `0xB0` `0xB1` `0xB2` | RX on CAN1 / CAN2 / CAN3 with timestamp | CAN frame payload with timestamp (timestamp mode on) |
| `0xB3` | RX on CAN3 with ESI and timestamp | |
| `0x8A` | STATUS | 38 bytes, every 250 ms, see 5.1 |
| `0x90` | FW_VERSION | ASCII, e.g. `31 2E 34 31` = "1.41" (*verified*) |
| `0x87` | MARKER | trigger input event; layout unknown |

The adapter never echoes transmitted frames back.

### 5.1 STATUS (0x8A)

Sent unsolicited every 250 ms. 38 payload bytes, 42 bytes on the wire
(`0F 2A 8A ... EF`). Byte numbers count from the first payload byte.
Known content (*verified* by forcing errors per port with mismatched
bitrates, and by burst tests against a blocked and a healthy bus):

| Byte | Content |
|---|---|
| 0 | `0x05`, constant (format version?) |
| 1 | CAN1 transmit error counter (TEC) |
| 2 | CAN1 receive error counter (REC) |
| 3 | CAN2 TEC |
| 4 | CAN2 REC |
| 5 | CAN3 TEC (*probable*: 0x08 seen while CAN3 transmitted without ACK) |
| 6 | CAN3 REC (*probable*: 0x40 at the same time) |
| 7 | CAN3 flags, 0x1D / 0x28 / 0x2E / 0x40 / 0x83 / 0x87 seen during CAN3 errors (*unknown bit meaning*) |
| 8 | 0x01 seen during CAN3 errors |
| 11-12 | frames on CAN3 in the last status period, little endian u16, both directions (*verified*: 1200 = `B0 04` at 4800 frames/s) |
| 13 | 0x12 / 0x14 seen during CAN3 errors |
| 15 | CAN1 transmit buffer fill in frames, 220 = full (*verified*) |
| 16 | CAN2 transmit buffer fill, same (*verified*) |
| 17 | CAN3 transmit buffer fill. The buffer holds 120 classic frames, about 102 24-byte FD frames (*verified*) |
| 18 | CAN1 receive overload: nonzero when frames toward the host were lost in the period (1 .. 4 seen) (*verified*) |
| 19 | CAN2 receive overload, same (*verified*) |
| 20 | CAN3 receive overload (*assumed* by position: CAN3 never lost frames in the tests) |
| 21 | CAN1 transmit overflow counter: host frames dropped because the transmit buffer was full, mod 256, since power-up (*verified*: equals the frames sent minus the frames received on the other side in burst tests) |
| 22 | CAN2 transmit overflow counter, same (*verified*) |
| 23 | CAN3 transmit overflow counter, same (*verified*) |
| 27 | 0x10, 0x13, 0x20, 0x30 observed, slowly rising over an hour (temperature? uptime?) |
| 29 | CAN1 state: 0 error active, 1 error passive (*verified*); bus off not observed |
| 30 | CAN2 state, same encoding |
| 31 | CAN3 state, same encoding (*probable*, stayed 0 in the tests) |
| others | 0 in every capture |

Earlier notes put a buffer level at byte 14 and a frame counter at byte
21 on the receiving adapter. Neither showed up again: they were the
transmitting adapter's bytes 15 and 21.

Observed sequences (sender without ACK, 250 ms apart): TEC `0x80` with
state 1, then TEC `0x80` REC `0x10`, then both 0 and state 0 again. The
controller recovers on its own. With a bitrate mismatch and continuous
sending, both sides end error passive with TEC 248 / REC 128 and stay
there. Bus off could not be provoked without a physical fault.

#### Transmit flow control

The adapter accepts frames from the host faster than any bus takes them,
and drops what does not fit into the transmit buffer (bytes 21 .. 23
count them). The driver therefore keeps the queue of a port within the
buffer:

* until the first status: three quarters of an empty buffer;
* after each status: up to three quarters of the buffer at once
  (3/4 capacity - fill), plus three quarters of what the bus drained in
  the last period (last fill + frames sent - frames dropped - fill),
  released evenly over the next 250 ms.
* The capacity of the CAN3 buffer depends on the frame sizes. The driver
  learns it from the fill at which frames were dropped (not below half
  the nominal size).

Two first versions lost frames. Releasing the drained share at once
overflowed the buffer in the middle of the period, because the USB link
is faster than the bus. Filling the buffer up to 10 frames short of full
lost 16 frames on a 250 kbit/s bus at 95 % load, whose drain rate varied
by 20 % between periods.

Measured with firmware 1.41 through usbipd: 0 frames dropped out of
2000 .. 10000 per run in these cases:

* a blocked CAN1;
* a blocked CAN3 with 8, 24 and 64-byte FD frames;
* a healthy CAN1 at 1 Mbit/s, about 2100 frames/s, including the first
  run after loading the module;
* CAN1 at 250 kbit/s with the bus full.

Before, the adapter dropped everything beyond 220 frames on a blocked
bus. The cost is throughput: the first version, which filled the buffer
further, reached about 2600 frames/s at 1 Mbit/s. Dropped frames
still count as `tx_dropped` with a `CAN_ERR_CRTL_TX_OVERFLOW` error frame,
and receive overloads as `rx_over_errors` with `CAN_ERR_CRTL_RX_OVERFLOW`.

## 6. Behaviour notes

* Three CAN ports share one byte stream. Frames for different ports can
  be interleaved freely.
* The adapter does not acknowledge commands. The settings command takes
  effect immediately.
* Receive rate toward the host saturates between about 2000 and 5000
  frames/s per adapter (measured through usbipd into WSL2); beyond that the
  firmware drops frames (bytes 18 .. 20 of the status flag it). Transmit
  is limited by the bus and the transmit buffer (5.1).
* With a single other node in listen-only mode (no ACK), the adapter keeps
  retransmitting the frame indefinitely, as CAN requires.
* A message longer than the firmware's buffer locks the host-to-bus path
  up until power cycle (section 2.2).

## 7. Captured examples (firmware 1.41)

All bytes hex, exactly as on the wire.

| Direction | Bytes | Meaning |
|---|---|---|
| host → adapter | `0F 08 8D 00 00 00 01 EF` | buffer mode 1 |
| host → adapter | `0F 05 8B 01 EF` | timestamp mode on |
| host → adapter | `0F 08 88 01 00 FA 00 EF` | CAN1 250 kbit/s |
| host → adapter | `0F 08 88 02 01 F4 00 EF` | CAN2 500 kbit/s |
| host → adapter | `0F 0C 88 03 00 13 2C FC 00 00 00 EF` | CAN3 FD 125k/6.7M |
| host → adapter | `0F 04 90 EF` | firmware version request |
| adapter → host | `0F 08 90 31 2E 34 31 EF` | firmware "1.41" |
| host → adapter | `0F 0D A0 00 00 03 21 04 AA BB CC DD EF` | TX CAN1, id 0x321, 4 bytes |
| host → adapter | `0F 0F A0 00 00 01 23 06 11 22 33 44 55 66 EF` | TX CAN1, id 0x123, 6 bytes: 15-byte message, raw length 0x0F |
| host → adapter | `0F 10 A0 1A BC DE F0 84 1F 0F 1F 1F 1F EF 00 EF` | TX CAN1, extended 0x1ABCDEF0, data 0F 1F EF 00 escaped |
| adapter → host | `0F 0E A1 1A BC DE F0 84 DE AD BE 1F EF EF` | RX CAN2, extended, data DE AD BE EF, no timestamp |
| adapter → host | `0F 15 B0 00 00 01 23 08 00 29 35 82 11 22 33 44 55 66 77 88 EF` | RX CAN1 with timestamp 0x00293582 µs |
| adapter → host | `0F 19 B2 00 00 01 00 39 00 37 0D 15 00 01 .. 0B EF` | RX CAN3, FD, BRS, 12 bytes, timestamp |
| adapter → host | `0F 2A 8A 05 80 08 00 .. 00 20 00 01 00 .. EF` | status: CAN1 TEC 128 REC 8, state 1 |
