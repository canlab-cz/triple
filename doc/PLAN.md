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
- [x] `TripleSendHex` escapes the length byte when the total length is 0x0F / 0x1F / 0xEF. v1.3 wrote it with `PushByte` at offset 1, which overwrote the command byte for exactly those lengths (a plain 6-byte standard frame is 15 bytes long).
- [x] Driver `Makefile` uses `$(M)/include` and `CURDIR`, so `make -C driver` works from any cwd.
- [x] Daemon: tokenised argument parsing with error messages, default bitrates when `-s` is absent, `-v` prints the version, `N_TRIPLE` shared with the driver. Stale `utility/.depend64` removed, `.gitignore` added, `start.sh` double `sudo` fixed.
- [x] Build matrix in containers (`ci/kbuild/run.sh`): Ubuntu 22.04 (5.15), Ubuntu 24.04 (6.8), Fedora 44 (7.2): all build without warnings. GitHub Actions workflow `.github/workflows/build.yml`.
- [x] Host-side codec round-trip test (`tests/codec`): encode -> escape -> decode for CAN 2.0 and FD, RTR, all special bytes, special-valued length bytes, malformed input.
- [x] README updated.
- [ ] Hardware verification with two adapters back to back:
  - RX identifier polarity: does the firmware set flag bit 0x80 for extended ids? (`driver/tx.c` keeps the v1.3 inverted interpretation.)
  - Does the firmware escape the length byte on frames it sends (escaped length 15 or 31), and does it accept an escaped length byte on TX?
  - TX under load on two ports at once; FD frames with 64 data bytes of 0x0F / 0x1F / 0xEF.
  - USB unplug while attached, then re-plug and restart.
  - `ip -details -s link` counters.

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
