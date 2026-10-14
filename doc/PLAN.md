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

- `alloc_candev` / `register_candev` / `open_candev` / `free_candev`; `alloc_can_skb` / `alloc_canfd_skb`.
- Bitrate, listen-only and FD via `can_priv` and netlink: `ip link set can0 type can bitrate 250000 [dbitrate ... fd on]`. The kernel sends the settings commands itself. Daemon reduces to `ldattach 29 /dev/ttyACM0` (or disappears).
- Per-port TX state or a shared TX ring; echo-skb API for correct tx stats.
- Enable timestamp mode; deliver `skb_hwtstamps`; report error counters / bus-off via `can_change_state`.
- Drop pre-5.10 compatibility code.
- udev rule + systemd unit for automatic attach on plug.
- Later option: native USB driver on the bulk endpoints (like `gs_usb`), which needs a `cdc_acm` ignore entry or udev unbind.

### Phase 3: protocol documentation and shared codec

- `doc/protocol.md` written from the code (all commands, settings payloads, FD DLC mapping, escaping).
- Single header-only codec shared by driver, daemon and GUI, with host-side unit tests.

### Phase 4: analyzer GUI (PP2CAN-like)

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
