# USB2CAN Triple: refactoring and analyzer plan

Status as of 2026-10-14. Mainline Linux is 7.3-rc; the driver was last tested on 5.15.

## 1. Current state

| Part | Files | Notes |
|---|---|---|
| Kernel module `usb2cansocketcan.ko` | `driver/` | tty line discipline derived from slcan / Innodisk EMUC. Creates 3 netdevs (2x CAN 2.0, 1x CAN FD). |
| Daemon `tripled` | `utility/` | Opens `/dev/ttyACMx`, sends bitrate / mode commands, attaches the ldisc, renames interfaces, sleeps. |
| Scripts | `start.sh`, `end.sh`, `bootexec/` | Manual load/unload, untested SysV init scripts. |

Hardware: Microchip PIC based, USB VID `04d8` PID `f0d7`, enumerates through `cdc_acm`.
Framing: `0x0F <len> <cmd> <payload...> 0xEF`, escape byte `0x1F` precedes any payload byte equal to `0x0F`, `0xEF` or `0x1F`.

### Kernel API breaks (verified against current `include/linux/tty_ldisc.h`)

| Problem | Location | Since |
|---|---|---|
| `NR_LDISCS - 1` is 30 = `N_CAN327`; registration fails when can327 is loaded. Use `N_DEVELOPMENT` (29). | `driver/main.c` | 5.19 |
| `receive_buf(tty, const u8 *cp, const u8 *fp, size_t count)` | `driver/main.c` | 6.6 |
| ldisc `ioctl` and `tty_mode_ioctl` lost the `struct file *` argument | `driver/main.c` | 5.16 |
| ldisc `hangup` returns `void` | `driver/main.c` | 5.16 |
| `netif_rx_ni()` removed, use `netif_rx()` | `driver/tx.c` | 5.18 |
| `can_frame.can_dlc` deprecated, use `len` | `driver/tx.c` | 5.11 |

### Defects found in review

- `triple_close` guard is commented out: hangup + close on USB unplug dereferences NULL (kernel oops). This is why `end.sh` must be run before unplugging.
- RX buffer overflow: `rcount` keeps growing past `TRIPLE_MTU`; the next end byte calls `escape_memcpy` with the oversized count.
- `cf` / `cf_fd` in `triple_bump` are never zeroed; FD path reads `cf.can_dlc` from the unused struct for stats; short frames leak stack bytes to userspace.
- One `xbuff` shared by three netdevs: concurrent xmit on two ports overwrites pending bytes.
- `devs[!channel]` in `triple_netdev_close` is a 2-port leftover.
- Timestamped RX commands `0xB0..0xB3` and marker `0x87` are not decoded; daemon disables timestamp mode, so 1 us hardware timestamps are lost.
- Daemon: ~11 s of `sleep()` at startup, `-v` prints nothing, `-s`/`-l`/`-c` with too few tokens dereference NULL, stale `utility/.depend64` committed.

## 2. Phases

### Phase 1: quick compatibility fix (keep architecture)

Done 2026-10-14 (driver v1.4, daemon v1.1), not yet verified on hardware:

- [x] Version-guard the six API breaks above; `N_TRIPLE` fixed to 29 (`N_DEVELOPMENT`), `MODULE_ALIAS_LDISC(29)`.
- [x] Crash fixes: close guard restored (USB unplug), RX overflow (resync on next terminator, explicit escape state `SLF_ESC`), frames built with `alloc_can_skb` / `alloc_canfd_skb`, 3-port close logic, `NETDEV_TX_BUSY` while the shared TX buffer drains, FD frames rejected on ports 1 and 2.
- [x] `TripleRecvHex` validates frame length and DLC and rejects unknown commands (timestamped frames are dropped instead of being delivered on port 0).
- [x] Frame buffers enlarged from 100 to 160 bytes: a CAN FD frame whose 64 data bytes all need escaping is 144 bytes on the wire and overflowed `comm_buf` / `rbuff` / `xbuff` in v1.3.
- [x] Length byte (byte 1) is written and read RAW. v1.3 wrote it with `PushByte` at offset 1, which overwrote the command byte when the length was 0x0F / 0x1F / 0xEF (a plain 6-byte standard frame is 15 bytes long), so such frames never left the adapter.
- [x] Driver `Makefile` uses `$(M)/include` and `CURDIR`, so `make -C driver` works from any cwd.
- [x] Daemon: tokenised argument parsing with error messages, default bitrates when `-s` is absent, `-v` prints the version, `N_TRIPLE` shared with the driver. Stale `utility/.depend64` removed, `.gitignore` added, `start.sh` double `sudo` fixed.
- [x] Build matrix in containers (`ci/kbuild/run.sh`): Ubuntu 22.04 (5.15), Ubuntu 24.04 (6.8), Fedora 44 (7.2): all build without warnings. GitHub Actions workflow `.github/workflows/build.yml`.
- [x] Host-side codec round-trip test (`tests/codec`): encode -> escape -> decode for CAN 2.0 and FD, RTR, all special bytes, special-valued length bytes, malformed input.
- [x] README updated.
- [x] Hardware verification with two adapters back to back (WSL2 6.6.87.2, both adapters attached through usbipd-win, 2026-10-14), see "Protocol facts verified on hardware" below.
- [x] TX under load: 2000 frames on port 1 (250k) and 2000 on port 2 (500k) at the same time with 1 ms gap, and a 500-frame zero-gap burst: all received on the second adapter, no sequence gaps, no kernel warnings.
- [x] USB unplug of both adapters while attached with daemons running: no oops, all interfaces torn down, module reference count back to 0. Re-plug needs `usbipd attach` again and a daemon restart.
- [x] tx_packets accounting fixed (was counted per tty wakeup, undercounting under load).

#### Protocol facts verified on hardware (firmware as shipped on SN 2604002 and its sibling)

- Flag byte bit 0x80 means extended identifier in BOTH directions. v1.3 inverted it on receive, so every received frame had the wrong ID type.
- Byte 1 is the raw wire length (escapes included) and is NOT escaped in either direction. The firmware reads it as a plain byte count: an escaped length `1F 10` is taken as "31 bytes" and swallows the following frame as well. A 15-byte frame carries a literal `0F` in byte 1 and is accepted.
- Data bytes, identifier bytes and the flag byte ARE escaped with `1F` in both directions.
- The firmware sends a status frame (`0x8A`, 42 bytes) every 250 ms on its own. The driver ignores it; Phase 2 should decode it for error counters and bus state.
- CAN 2.0 on ports 1 and 2 (250 kbit/s and 500 kbit/s): every dlc 0..8, standard and extended ids, all special data bytes, both directions.
- CAN FD on port 3 (125 kbit/s nominal, 6.7 Mbit/s data): dlc 8, 12, 48, 64 with and without BRS, both directions, up to 100 bytes on the wire.
- A 137-byte frame from the host (dlc 64, every data byte escaped) locks up the adapter's host-to-bus path. Status frames keep coming, a USB reset and re-sending the settings commands do not recover it, only a power cycle does. The driver now drops frames longer than `max_wire_len` (module parameter, default 100, which is the largest size verified) with `tx_dropped++`. The real firmware limit is somewhere in 101..136 and should be read from the firmware source rather than bisected (each miss costs a power cycle). 100 bytes covers dlc 64 with up to 27 escaped data bytes.
- Status frame payload (38 bytes after the command byte) is not decoded. Observed: byte 0 = 0x05, byte 6 went 0x00 -> 0x04 after traffic, byte 27 = 0x10 on one adapter and 0x13 on the other after the FD lock-up (candidate for CAN3 error state). To be mapped against the firmware source in Phase 2.

### Phase 2: rewrite as a modern CAN device driver (template: in-tree `drivers/net/can/slcan`)

Done 2026-10-14, driver v2.0 (`driver/triple.c`, `driver/triple_proto.c`), verified on two adapters:

- [x] `alloc_candev` / `register_candev` / `open_candev` / `free_candev`, `alloc_can_skb` / `alloc_canfd_skb`, echo skbs with `IFF_ECHO` (tx counters exact).
- [x] Bitrates and modes via netlink (`bitrate_const`, `data_bitrate_const`, `CAN_CTRLMODE_LISTENONLY | FD | FD_NON_ISO`); the driver sends the settings command in `ndo_open`. Invalid values are refused by the CAN core.
- [x] Attach with `ldattach 29 /dev/ttyACMx`; the `tripled` daemon, `start.sh`, `end.sh` and the SysV scripts are gone. udev rule + systemd template unit in `etc/`, helper scripts in `scripts/`, `driver/dkms.conf`.
- [x] Timestamp mode on by default: 32-bit microsecond counter after the flag byte, extended to 64 bit in the driver, delivered as `skb_hwtstamps` (`candump -H`). Measured 102 ms deltas for 100 ms spacing.
- [x] Status message decoded: TEC/REC per port into `do_get_berr_counter`, state changes delivered as error frames via `can_change_state`, `can_bus_off` on the (unobserved) bus-off state byte. Firmware version logged at attach.
- [x] Interfaces carry `dev_port` 0..2 for udev naming by connector.
- [x] Builds on 5.15, 6.6 (WSL2), 6.8 and 7.2 (6.16 moved the FD parameters into `can_priv::fd`, 7.0 removed `can_change_mtu` and replaced `can_eth_ioctl_hwts` by `ndo_hwtstamp_get/set`).
- [x] Simulated unplug (usbipd detach) with ports up: clean teardown, the adapter re-enumerates and can be re-attached.
- [ ] User defined bit timing for port 3 (`TRIPLE_CMD_BITTIMING`, raw MCP2517FD values) through `bittiming_const` / `do_set_bittiming`.
- [ ] Marker input (`0x87`) and the rest of the status payload (byte 21 changes under load, byte 27 looks like a slowly rising value, 0x10 .. 0x30).
- Later option: native USB driver on the bulk endpoints (like `gs_usb`), which needs a `cdc_acm` ignore entry or udev unbind.

Measured limits (firmware 1.41, WSL2 over usbipd, so USB latency is pessimistic):

| Test (8-byte frames, 250 kbit/s unless noted) | Received |
|---|---|
| 2 x 2000 frames on two ports at once, 1 ms gap | 2000 / 2000 on both |
| 1000 frames, 0.5 ms gap (2000 frames/s) | 1000 / 1000 |
| 1000 frames, 0.2 ms gap (5000 frames/s) | 923 / 1000 |
| 1000 frames, 0.13 ms gap (bus rate) | 768 / 1000 |
| 1000 frames, zero gap | 385 / 1000 |
| 1000 frames zero gap at 500 kbit/s | ~530 / 1000 |
| 200 FD frames x 64 bytes, BRS 6.7 Mbit/s, zero gap | 200 / 200 |

Same numbers with `hwtstamp=0`, so the message length is not the limit. The sending adapter puts everything on the bus; the *receiving* adapter's path toward USB saturates somewhere between 2000 and 5000 frames/s and the firmware drops silently once its buffer is full. During overload the receiver's status payload shows byte 14 rising to about 0xDC (buffer fill level, probably) and byte 21 counting; both are 0 at idle. v1.4 passed a 500-frame zero-gap burst only because it waited for each USB write completion, which throttled the sender below the receiver's limit by accident. All measured through usbipd-win into WSL2; native USB should do better.

### Phase 3: protocol documentation and shared codec

Done 2026-10-14:

- [x] `doc/protocol.md`: transport, framing and escaping (raw length byte), size limits, CAN frame payload with flags, DLC and timestamp, every command in both directions with payload layouts, the speed code table and formula, the raw bit timing layout (from v1.x, unverified), the decoded parts of the status message, behaviour notes and the captured byte sequences. Everything is labelled verified / probable / unverified.
- [x] Shared codec `driver/triple_proto.[ch]`: C, no kernel dependencies, compiled into the module and by `tests/codec` on the host. Added `triple_encode_bittiming_fd()` for the raw MCP2517FD timing command so the open Phase 2 item only needs the netlink glue.
- [x] Python reference implementation `tools/triple_proto.py` (codec, stream parser, bitrate tables) with `tools/test_triple_proto.py`; the C and Python tests share the captured vectors and the bit timing vector, so the two implementations are kept in step.
- [x] `tools/triple_dump.py`: candump-like tool that drives an adapter directly over the serial port with pyserial, on Linux, Windows or macOS, without the kernel module. This is the seed of the GUI's direct-serial backend (Phase 4).
- [ ] Remaining unknowns in the protocol need the firmware source: status bytes 7, 8, 13, 14, 21, 27 and the bus-off encoding, the marker message, the real host-side buffer size, and what buffer mode selects.

### Phase 4: analyzer GUI (PP2CAN-like)

Decision 2026-10-14: build on SavvyCAN (MIT, Qt). First milestone done the same day, see `gui/savvycan/`:

- [x] New connection type `CANCon::TRIPLE` ("CANLAB USB2CAN Triple (serial)"): `gui/savvycan/connections/triple_serial.[h,cpp]` on top of the shared codec; `apply.py` copies the sources and edits 8 upstream files (project file, type enum, factory, dialog, connection window); `savvycan-upstream.patch` is the same diff.
- [x] Three buses per adapter, bitrates / listen-only / FD data rate / non-ISO from the connection window, adapters listed first in the port list by USB id, 100-byte wire limit enforced, error counters and firmware version in the device console.
- [x] Receive with the adapter's hardware timestamps mapped onto SavvyCAN's manager-relative clock (103 ms spacing shown for 100 ms sends), transmit through the frame sender (13 of 13 frames at 200 ms received by the kernel driver on the second adapter), CAN FD frame on bus 2, extended ids.
- [x] Built with Qt 5.15 on Ubuntu 22.04 under WSL2 and exercised on the WSLg display with xdotool-driven screenshots (`doc/img/savvycan-triple.png`).
- [x] Windows build: SavvyCAN's `QT6WIP` branch with Qt 6.8.1 / MinGW 13.1 on Windows 11; `apply.py` handles the CMake source list and the branch's own FD-capable flag, a typedef layer covers `CANFrame` (master) versus `CommFrame` (QT6WIP).
- [x] Fixed a crash found on Windows: `QSerialPort::open()` emits its error signal synchronously, the error slot deleted the port, and the caller then dereferenced it. The error signal is now connected only after a successful open. Removed an unused timer that triggered a cross-thread parent warning.
- [ ] Windows end-to-end traffic test: adapter B needs a physical replug after its usbipd round trip (Windows could not open it, semaphore timeout, also from .NET).
- [ ] Bus-off / error frames into SavvyCAN's frame list (currently console only), marker input, status fields once the firmware source clarifies them.
- [ ] Upstream pull request to collin80/SavvyCAN once the Windows build is confirmed.
- [ ] PP2CAN feature gap review: what SavvyCAN already covers (logs, DBC, graphs, replay, sender, scripting, ISO-TP/UDS) versus what needs adding (CANopen tools, reply maker, gateway, HW sync).

Original design notes:

Backend abstraction with two implementations:

1. SocketCAN backend (Linux, any adapter).
2. Direct serial backend speaking the Triple protocol over the CDC port: works on Windows/macOS without a kernel module, gets hardware timestamps and the marker input.

Stack: Qt 6 / C++ (throughput at 3 ports, 8 Mbit FD), reusing the Phase 3 codec. Evaluate building on SavvyCAN (GPL, Qt, multi-bus logs, DBC, graphing, replay, scripted sender, pluggable serial connections) before starting from scratch.

MVP order: live RX/TX grid with filters and pause -> manual and periodic transmit -> message database save/load -> file logging and replay -> bus load and ID trap -> data/bit senders and receivers -> reply maker -> CANopen tools.

PP2CAN feature map (from canlab.cz/cs/pp2can_seznameni): RX log, TX log, info window, predefined message database, Sync / HW sync periodic senders, Data sender/receiver, Bit sender/receiver, ASCII sender/receiver, File sender (replay), Signal sender/receiver, Grid log, Filtered log, Log filter, Find, Multipacket sender, Timing accuracy, Response meter, ID trap, Bus load, Msg. filter, File logging with markers, Save log / Print, Fault injector, CANopen tools, Reply maker, Gateway, Set/Get number, CAN calculator, Plugins, Listen only, Loopback.

### Phase 5: packaging

- DKMS package for the module.
- CI compiling against several kernel header sets.
- GitHub release workflow for the GUI on Linux and Windows.

## 3. Open decisions

- GUI: build on SavvyCAN or start fresh.
- Phase 2: keep the ldisc or move to a native USB driver.
