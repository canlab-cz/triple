# USB2CAN Triple: SocketCAN driver, CAN analyzer and reverse engineering tools

Software for the CANLAB **USB2CAN Triple** adapter: three CAN connectors
on one USB port, CAN1 and CAN2 classic CAN, CAN3 CAN FD
([canlab.cz](https://www.canlab.cz)).

* **Linux driver**: the adapter as three SocketCAN interfaces
  (`can0` .. `can2`) on kernels 5.15 to 7.x, with hardware timestamps,
  CAN FD, any bit timing on CAN3, bus state reporting and transmit flow
  control.
* **CAN analyzer**: [SavvyCAN](https://github.com/collin80/SavvyCAN) with
  a native Triple connection for Windows and Linux, plus PP2CAN-style
  tools: Bus Statistics, CANopen (SDO, PDOs from EDS, block transfer,
  LSS), Data Panel, Response Meter, CAN Calculator, PP2CAN log files,
  Print Log.
* **Reverse engineering**: Signal Finder finds counters, checksums,
  signals and multiplexers and writes DBC files; the AI Assistant
  answers questions about a capture through Claude, with limits you set
  on tokens, requests and bus access.

![Signal Finder decoding a simulated engine message](doc/img/re-signal-finder-engine.png)

## Quick start

### Linux: SocketCAN

```sh
git clone https://github.com/canlab-cz/triple && cd triple
make -C driver
sudo insmod driver/usb2cansocketcan.ko
sudo sh scripts/triple-attach.sh ttyACM0    # can0 250k, can1 500k, can2 FD 125k/6.7M
candump -H any
```

Details, bitrates, CAN FD timing, udev rules, WSL2:
[doc/linux-driver.md](doc/linux-driver.md).

### Windows or Linux: SavvyCAN

```sh
git clone --branch QT6WIP https://github.com/collin80/SavvyCAN   # Windows, Qt 6
git clone https://github.com/collin80/SavvyCAN                   # Linux, Qt 5
python3 gui/savvycan/apply.py SavvyCAN
```

Build it ([gui/savvycan/README.md](gui/savvycan/README.md#build)), then
Connection > Open Connection Window > Add New Device Connection >
"CANLAB USB2CAN Triple (serial)". No driver is needed on Windows. User
guide: [doc/savvycan-guide.md](doc/savvycan-guide.md).

### Reverse engineering, without hardware

```sh
python3 tools/re_demo.py demo.log          # a minute of simulated car traffic
make -C tools/canre
tools/canre/canre_cli demo.log analyze_id id=0C9
```

```text
  signal   start  5 len 14 Motorola raw 3140..13039, 2548 values, Motorola byte order (high byte first)
  signal   start 16 len  8 Intel    raw 0..162, 93 values
  signal   start 24 len  8 Intel    raw 65..130, 66 values
  constant start 32 len  8 Intel    raw 0..0, 1 values
  constant start 40 len  8 Intel    raw 60..60, 1 values
  counter  start 48 len  4 Intel    raw 0..15, 16 values, +1 per frame in 100 % of frames, 4 bits
  checksum start 56 len  8 Intel    raw 0..255, 256 values, CRC-8 SAE J1850 of the other bytes
```

RPM, throttle, coolant temperature, two constants, a rolling counter and
the checksum. Load `demo.log` in SavvyCAN and open RE Tools > Signal
Finder or AI Assistant. Walkthrough with AI Assistant examples:
[doc/reverse-engineering.md](doc/reverse-engineering.md).

## Screenshots

| | |
|---|---|
| ![SavvyCAN with a USB2CAN Triple on Windows](doc/img/savvycan-triple-windows.png) | ![Connection window with CAN FD settings](doc/img/savvycan-connection-window.png) |
| SavvyCAN receiving from the Triple | Connection window: per-bus settings and device console |
| ![Multiplexed message, one page decoded, signed current plotted](doc/img/re-signal-finder-mux.png) | ![Comparing the seconds before and after a door opened](doc/img/re-compare-door.png) |
| Signal Finder: a multiplexed message, page 0 | What changed when the door opened |
| ![AI Assistant](doc/img/ai-assistant.png) | ![AI Assistant limits](doc/img/ai-assistant-limits.png) |
| AI Assistant on a capture | API key and limits |
| ![Bus Statistics](doc/img/savvycan-bus-statistics.png) | ![CANopen PDOs decoded from the EDS](doc/img/savvycan-canopen-pdo.png) |
| Bus Statistics: load, errors, overflows, TEC/REC | CANopen: PDOs decoded live from the EDS |
| ![CANopen LSS](doc/img/savvycan-canopen-lss.png) | ![Data Panel](doc/img/savvycan-data-panel.png) |
| CANopen LSS: fastscan and node-ID | Data Panel: sliders and bits per byte |
| ![Print preview](doc/img/savvycan-print-log.png) | ![Playback of a PP2CAN log](doc/img/savvycan-file-sender.png) |
| Print Log with preview | Playback of a PP2CAN log |

## Documentation

| Document | Content |
|---|---|
| [doc/linux-driver.md](doc/linux-driver.md) | Build, attach, bitrates, CAN FD bit timing, errors and statistics, flow control, udev, WSL2, module parameters, limits |
| [doc/savvycan-guide.md](doc/savvycan-guide.md) | SavvyCAN with the Triple: connection, capture, sending, Bus Statistics, CANopen, Response Meter, CAN Calculator, printing |
| [doc/reverse-engineering.md](doc/reverse-engineering.md) | Signal Finder, window comparison, multiplexers, DBC export, AI Assistant setup, limits and examples, `canre_cli` |
| [gui/savvycan/README.md](gui/savvycan/README.md) | How the SavvyCAN changes are applied and built (Windows and Linux) |
| [doc/protocol.md](doc/protocol.md) | The adapter's USB protocol: framing, commands, status message, measured behaviour |
| [doc/pp2can-gap.md](doc/pp2can-gap.md) | PP2CAN features and where they are in SavvyCAN |
| [doc/PLAN.md](doc/PLAN.md) | Roadmap and test record |

## Repository layout

| Path | Content |
|---|---|
| `driver/` | Kernel module (`triple.c`) and the shared protocol codec (`triple_proto.[ch]`) |
| `gui/savvycan/` | SavvyCAN connection, tool windows, `apply.py` and the upstream patch |
| `tools/` | Python codec and `triple_dump.py` (adapter without the kernel module), CANopen node simulator, `re_demo.py`, `canre/` command line analysis |
| `scripts/`, `etc/` | Attach / detach scripts, udev rule, systemd unit |
| `tests/` | Codec tests (C and Python, with captured firmware messages), unit tests of the tool windows' logic |
| `ci/kbuild/` | Kernel build matrix in Docker |
| `doc/` | Documentation and screenshots |

## Development

```sh
make -C tests/codec test                 # C codec, captured firmware vectors
python3 tools/test_triple_proto.py       # same vectors, Python codec
make -C tests/gui test                   # analysis, CANopen, AI tools, print layout
sh ci/kbuild/run.sh                      # the driver against several kernels (Docker)
```

Driver v1.x (line discipline plus the `tripled` daemon, kernels up to
5.15) is in the git history before tag `v2.0`.

## Status

Verified on two adapters wired back to back (firmware 1.41), on Linux
(native and WSL2) and Windows 11. Not decoded yet: the trigger/marker
input, the firmware's bus-off indication, and hardware-timed periodic
sending; these need the firmware documentation. The AI Assistant has
been tested against a local mock of the Anthropic Messages API, not yet
against the real service.

## License

Each source file states its license in an `SPDX-License-Identifier`
line; the full texts are in `LICENSES/` and `LICENSE` lists the
assignment.

| Part | License |
|---|---|
| Kernel module `driver/triple.c` | GPL-2.0 (required for a Linux kernel module) |
| Protocol codec `driver/triple_proto.[ch]`, SavvyCAN additions `gui/savvycan/`, tools, tests | MIT |

MIT is compatible with the GPL, so the module as a whole is GPL-2.0
while the codec can also be used in MIT projects such as SavvyCAN.
