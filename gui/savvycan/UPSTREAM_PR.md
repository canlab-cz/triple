# Upstream pull request to collin80/SavvyCAN (draft)

Status: prepared locally, **not opened**. Branch `usb2can-triple` in the
WSL checkout `/home/pavel/SavvyCAN`, on top of upstream master `e439670`,
has ten commits:

| Commit | Content |
|---|---|
| `5076eab` | Add a connection type for the CANLAB USB2CAN Triple |
| `a85db3c` | Add a Bus Statistics window (generic, works with every connection) |
| `7997283` | Add a CANopen window (generic: NMT, node monitor, EMCY, SYNC, SDO client) |
| `ec64818` | Add PP2CAN log files, Data Panel, Response Meter and CAN Calculator |
| `8daedb1` | Add PDO decoding to the CANopen window (EDS / DCF or mapping read over SDO) |
| `f67667d` | Add Print Log to the File menu |
| `3a41e68` | Triple: zero padding for CAN FD lengths between DLC steps |
| `a5f04bf` | Triple: CAN3 bitrate pairs outside the firmware table as raw timing |
| `6ffd6aa` | Add Signal Finder and AI Assistant windows (reverse engineering; can be left out or split) |
| `24d6c0b` | CANopen window: SDO block transfer and LSS |
| `f2d0908` | Triple: report frames lost in the adapter; Overflows in Bus Statistics |
| `a1e9132` | Signal Finder / AI tools: byte order from carries, multiplexer refinement, signed fields |

They can go in one PR or as separate ones; only the first commit is
specific to the Triple, the tool windows and the PP2CAN log format work
with every connection.

## Before opening

1. License: resolved. All four added files (`triple_serial.*`,
   `triple_proto.*`) are MIT (`SPDX-License-Identifier: MIT`, Copyright
   CANLAB s.r.o.), matching SavvyCAN. The kernel module that shares the
   codec stays GPL-2.0, which MIT is compatible with.
2. Fork collin80/SavvyCAN on GitHub, push the branch to the fork, open the
   PR against `master`.
3. Optionally a second PR against `QT6WIP`: the same files apply there
   with `apply.py` (CMakeLists entry instead of the .pro file, no
   connection-window edit).

## Title

Add a connection type for the CANLAB USB2CAN Triple

## Description

The [USB2CAN Triple](https://www.canlab.cz) from CANLAB s.r.o. is a USB
adapter with three CAN ports: CAN1 and CAN2 classic CAN, CAN3 CAN FD
(MCP2517FD). It enumerates as a CDC ACM serial port (USB `04d8:f0d7`), so
this connection drives it through `QSerialPort` on Linux, Windows and
macOS without a driver.

What the connection does:

- three buses per adapter; bitrate, listen only and (bus 2) CAN FD data
  rate are set from the connection window
- receive with the adapter's 1 µs hardware timestamps, mapped onto the
  connection manager's time base
- transmit on all buses, CAN FD with BRS on bus 2; frames above the
  adapter firmware's message size limit are refused with a console
  message instead of being sent
- error frames in the frame list on bus state changes (warning, passive,
  bus off, back to active) from the adapter's status messages, with
  TEC/REC in bytes 6 and 7
- Triple ports are listed first in the new connection dialog

Second commit: a **Bus Statistics** window (RE Tools menu) for every
connection: per bus bitrate, frames/s, load % (frame bus time at the
nominal and, for CAN FD with BRS, the data bitrate; stuffing not
counted), peak, error frames, bus state with TEC/REC from error frames,
and a 120 s load graph.

Files:

| File | Content |
|---|---|
| `connections/triple_serial.h/.cpp` | the `CANConnection` (`CANCon::TRIPLE`) |
| `connections/triple_proto.h/.c` | adapter wire protocol codec in plain C, shared with the Linux SocketCAN driver; protocol documented in canlab-cz/triple `doc/protocol.md` |
| `SavvyCAN.pro`, `canconconst.h`, `canconfactory.cpp`, `canconnectionmodel.cpp`, `connectionwindow.cpp`, `newconnectiondialog.*`, `ui/newconnectiondialog.ui` | registration, 54 lines |

All added files are MIT licensed, like SavvyCAN.

Compatibility: the connection compiles on master (Qt 5.15) and on
`QT6WIP` (Qt 6.8). A small typedef layer covers `CANFrame` versus
`CommFrame`; on QT6WIP bus 2 is marked with `setCanFDCapable(true)`.

Testing: two adapters wired back to back, one driven by this connection,
the other by the Linux SocketCAN driver.

| Platform | Result |
|---|---|
| Ubuntu 22.04, Qt 5.15, master | receive on all three buses incl. CAN FD, timestamps, transmit from the frame sender |
| Windows 11, Qt 6.8 MinGW, QT6WIP | receive 14/14 incl. extended IDs and a 12-byte FD frame with BRS; transmit 13/13 at 200 ms; error passive / error active frames on a forced bitrate mismatch |

🤖 Generated with [Claude Code](https://claude.com/claude-code)
