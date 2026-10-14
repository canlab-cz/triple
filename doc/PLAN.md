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
- [x] User defined bit timing for port 3: module parameter `fd_bittiming=1` registers the MCP2517FD limits (`bittiming_const`, 40 MHz clock) instead of the fixed table, so `ip link` takes any bitrate, sample point or time quanta; the driver sends command 0x89. Command verified on the bus 2026-10-14 (doc/protocol.md 4.2): register encoding (count minus one), 40 MHz, data phase applied, automatic TDC at 5M; kernel timing at 80 % / 80 % interoperates with the speed codes at 500k/2M, 250k/1M, 1M/5M, 125k/6.7M and reaches 800k/4M, which the table cannot; the kernel's default sample points lost BRS frames against the speed codes. Classic mode on port 3 works too. Conversion in the codec (`triple_fd_bittiming_from_tq`, `triple_fd_data_timing`), unit tested; `triple_dump.py` got `--can3-timing` and uses raw timing for CAN3 rates outside the table.
- [ ] SavvyCAN Triple connection: CAN3 pairs outside the firmware table (e.g. 100k/1M, 33k..100k nominal with FD from the connection window) now go out as raw timing at 80 % (`triple_fd_raw_for_rates`, unit tested; the same 100k/1M values work on the bus from `triple_dump.py`). Not yet confirmed on the bus through SavvyCAN's connection window: the scripted test did not apply the bus settings (device row probably not selected).
- [x] Found in those tests: frames whose length lies between two CAN FD DLC steps (10 bytes in a 12-byte frame) went out with the gap filled from the firmware buffer (bytes of an earlier frame). The C and Python codecs now pad with zeros; the kernel driver was not affected (the kernel rounds CAN FD lengths), the SavvyCAN connection and the Python tools were.
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
- [x] Windows end-to-end (2026-10-14): SavvyCAN on Windows 11 on COM7, the other adapter on the Linux kernel driver in WSL2. Receive 14/14 on all three buses including an extended id and a 12-byte FD frame with BRS, hardware timestamps 103 ms apart for 100 ms sends; transmit from the quick-send grid 13/13 at 200 ms (`doc/img/savvycan-triple-windows.png`). Note: SavvyCAN's grid parses unprefixed numbers as decimal, so ids and data need `0x`.
- [x] "NO-CARRIER for a second after attach" is not a driver issue: carrier is on for all three ports immediately (sampled every 100 ms), only `operstate` lags by about 0.9 s on a fresh attach. That is the kernel's link-watch rate limiting, which batches non-urgent link events for every network device. Scripts should not wait on `operstate` right after `ldattach`; sending works immediately.
- [ ] usbipd round trips leave an adapter unusable on Windows (open times out) until it is physically replugged; a `pnputil /restart-device` does not help. Worth a note for users who switch adapters between WSL2 and Windows.
- [x] Error frames in SavvyCAN's frame list: on every bus state change (active, warning at 96, passive at 128, bus off) the connection queues a SocketCAN-layout error frame (controller state in byte 1, TEC/REC in bytes 6/7, `ControllerError` / `BusOffError`). Verified on Windows with a bitrate mismatch on CAN1: passive with REC 128, then active again (`doc/img/savvycan-triple-errorframes.png`).
- [ ] Marker input and the remaining status fields once the firmware source clarifies them.
- [ ] Upstream pull request to collin80/SavvyCAN: prepared locally (branch `usb2can-triple`, commits `5076eab` connection, `a85db3c` Bus Statistics window and `7997283` CANopen window, `ec64818` PP2CAN tools, `8daedb1` CANopen PDO decoding, `f67667d` Print Log, `3a41e68` CAN FD zero padding, `a5f04bf` raw timing for non-table CAN3 pairs, `5f3585b` Signal Finder and AI Assistant in the WSL checkout, text in `gui/savvycan/UPSTREAM_PR.md`). License resolved 2026-10-14: the codec, the SavvyCAN connection, tools and tests are MIT; only `driver/triple.c` is GPL-2.0. Opening the PR publishes under the maintainer's GitHub account and is left to them.
- [x] PP2CAN feature gap review: `doc/pp2can-gap.md` maps every PP2CAN feature to SavvyCAN. Covered: logs, filters, search, continuous logging, periodic sender, playback, gateway (CAN Bridge), graphs, ID / timing analysis, DBC signals, scripting. Missing, ranked: bus statistics window (load %, TEC/REC over time), CANopen basics (NMT, heartbeat, SDO, PDO from EDS), response meter, slider-based data/bit sender, CAN calculator. Blocked on firmware source: HW-timed sync, markers, overflow counter, bus-off encoding.
- [x] Bus statistics window (RE Tools > Bus Statistics, `gui/savvycan/re/busstatswindow.*`): per bus bitrate, frames/s, load % from frame timing at the nominal and data bitrates, peak, error frames, last state with TEC/REC, load graph over 120 s. Works with any SavvyCAN connection. Verified on Windows: 897 frames/s at 250k read 39.8 % (= 897 x 111 bits), 472 frames/s at 500k 10.5 %, 50 FD frames of 64 bytes with BRS at 125k/6.7M 1.6 %; state and counters follow error frames (`doc/img/savvycan-bus-statistics.png`).
- [x] Fixed a timestamp bug found in that test (driver and SavvyCAN): frames from different ports can arrive slightly out of timestamp order, and any backward step was taken as a 32-bit counter wrap, adding 4295 s. The extension now lives in the shared codec (`triple_ts_extend`): up to 1 s backwards is reordering, a wrap needs a larger backward jump; unit tested in `tests/codec`.
- [x] CANopen basics (RE Tools > CANopen, `gui/savvycan/re/canopenwindow.*`): node monitor from heartbeat / boot-up / EMCY with decoded emergency codes, NMT master (start, stop, pre-operational, reset node, reset communication, per node or all), SYNC, SDO client with expedited and segmented upload and download, readable abort codes, one-click identity read (1000, 1008, 1009, 100A, 1018:1-4). Verified on Windows against `tools/canopen_sim.py` running on the Linux driver over the real bus (`doc/img/savvycan-canopen.png`).
- [x] CANopen PDO decoding (PDOs tab of the CANopen window): the mapping comes from an EDS / DCF file (CiA 306, `$NODEID` expressions, DCF ParameterValue over DefaultValue) or is read from the node over SDO (1400.., 1600.., 1800.., 1A00..; an EDS loaded afterwards only adds names and data types). Live table per mapped object: value by EDS data type (signed, REAL32/64, BOOLEAN, strings), raw bits, frame count, period from hardware timestamps. The EDS file and node are remembered. Parser and decoder are Qt-free in `gui/savvycan/re/canopen_eds.h`, unit tested in `tests/gui` against `tools/canopen_sim.eds`. The simulator gained TPDO1 (100 ms event timer), TPDO2 (on SYNC) and RPDO1. Verified on Windows over the real bus: TPDO1 period 100.4 ms, TPDO2 on each SYNC with the SYNC counter, RPDO1 frames from another node decoded (`doc/img/savvycan-canopen-pdo.png`).
- [ ] CANopen later: SDO block transfer, LSS.
- [x] File sender with PP2CAN logs: SavvyCAN loads PP2CAN `.log` files through its Load Log File dialog and autodetection (`gui/savvycan/pp2canlog.*`, format in `pp2canlogfmt.h`), so Playback replays them; File > Save Log as PP2CAN writes the capture back in that format. Verified on a real 1,007,234-line PP2CAN capture (73 MB): every line parses and formats back byte-identical; Playback with original timing sent 7,870 frames in 9 s with none missing or altered; frames logged in the same millisecond may leave in a different order (1,410 of 7,870, most likely the adapter sending queued frames by CAN priority). Finding: PP2CAN writes time fields unpadded, `45.4` is 45 s + 4 ms.
- [x] Data Panel (Send Frames menu): per byte slider, hex value and bit check boxes, 16-bit value in Intel or Motorola order, fill from ASCII, send on change and/or periodically (precise timer; the default coarse timer gave 109 ms instead of 100 ms on Windows).
- [x] Response Meter (RE Tools): watch mode from frame timestamps (0.484 ms average, 0.018 ms std dev for SDO request/response in the test) and send mode (host clock to frame timestamp, 5.9 ms average incl. USB latency).
- [x] CAN Calculator (RE Tools): 29-bit, 11+18, MCP2510/2515 SIDH/SIDL/EID8/EID0, SJA1000 ID1..ID4, in all directions; conversions unit tested (`tests/gui`).
- [x] Print Log (File menu, `gui/savvycan/logprinter.*`): prints the frame list as shown (visible columns in screen order, current filters) or only the selected frames, through a print preview, landscape, column widths fitted to the page with long cells (CAN FD data) wrapped, title, date and page numbers. Layout math is Qt-free in `logprintlayout.h`, unit tested in `tests/gui`. Verified on Windows (Qt 6.8): 26 frames incl. 64-byte FD on 2 pages, 5 selected of 18 frames on 1 page. Found on the way: the print preview replays pen state that was never changed, so with a dark theme the first lines came out white; the painter now sets its pen explicitly. A sleeping network printer can report a page of zero size; then the first installed printer with a page (e.g. Microsoft Print to PDF) is used. "Save Log as PP2CAN" and "Print Log" sit before Preferences instead of after Exit.
- [ ] Remaining from the gap list: HW sync / markers / overflow counter (need the firmware source).
- [x] Reverse engineering beyond PP2CAN (2026-10-14): Signal Finder window (RE Tools; per ID timing, bit flip heatmap, detected counters, checksums XOR/SUM/CRC-8 J1850/AUTOSAR, constants, flags and signal candidates segmented by bit flip rate after READ with byte order inference; DBC export; comparison of two time windows) and an AI Assistant window (Claude via the user's API key; tools overview / list_ids / analyze_id / get_frames / signal_values / compare_windows / changes_around / propose_dbc / send_frame). Assistant limits, all user settable: tokens per reply, tool calls per question, frames and characters per tool result, session and daily token budgets (checked before each request, reply size capped to the rest, daily count persisted), requests per minute (waits), transmit off by default with per-frame confirmation and an ID allowlist, optional cost display. Analysis in `re/canre.h`, tools and limits in `re/aitools.h`, Qt-free and unit tested. Tested against a mock of the Messages API that validates every request (headers, cached tools and system prompt, tool_use/tool_result pairing): full analysis conversation, DBC proposal, tool call limit, session budget stop, rate limit wait. Not yet run against the real API (needs the user's key).

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
