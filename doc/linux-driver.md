# Linux driver (SocketCAN)

The kernel module `usb2cansocketcan` turns a USB2CAN Triple into three
SocketCAN network interfaces. The adapter enumerates as a CDC ACM serial
port (`/dev/ttyACMx`, USB `04d8:f0d7`); the module is a tty line
discipline (number 29) attached to that port with `ldattach`. After that
everything is standard SocketCAN: `ip link`, can-utils, python-can,
Wireshark, SavvyCAN's SocketCAN connection.

* [Requirements](#requirements)
* [Build and install](#build-and-install)
* [Attach an adapter](#attach-an-adapter)
* [Bitrates](#bitrates)
* [Any bit timing on CAN3](#any-bit-timing-on-can3)
* [Bus state, errors and statistics](#bus-state-errors-and-statistics)
* [Transmit flow control](#transmit-flow-control)
* [Automatic attach and stable names](#automatic-attach-and-stable-names)
* [WSL2](#wsl2)
* [Module parameters](#module-parameters)
* [Limits](#limits)
* [Troubleshooting](#troubleshooting)

## Requirements

* Linux 5.15 or newer with CAN support (`can`, `can-raw`, `can-dev`,
  `cdc-acm`), as in most distribution kernels.
* Kernel headers: `linux-headers-$(uname -r)` (Debian, Ubuntu),
  `kernel-devel` (Fedora).
* `ldattach` (util-linux) and `can-utils`.

Verified builds: Ubuntu 22.04 (5.15), Ubuntu 24.04 (6.8), Fedora 44
(7.2), WSL2 (6.6); the container matrix is in `ci/kbuild/`. Hardware
tests ran on two adapters wired back to back, firmware 1.41.

## Build and install

```sh
git clone https://github.com/canlab-cz/triple
cd triple/driver
make
sudo insmod usb2cansocketcan.ko
```

To install it permanently with DKMS (rebuilt on kernel updates):

```sh
sudo cp -r driver /usr/src/usb2cansocketcan-2.0
sudo dkms add -m usb2cansocketcan -v 2.0
sudo dkms build -m usb2cansocketcan -v 2.0
sudo dkms install -m usb2cansocketcan -v 2.0
```

## Attach an adapter

```sh
sudo ldattach 29 /dev/ttyACM0
ip -br link show type can          # can0 can1 can2 appear
sudo ip link set can0 type can bitrate 250000
sudo ip link set can1 type can bitrate 500000
sudo ip link set can2 type can bitrate 125000 dbitrate 6700000 fd on
sudo ip link set can0 up
sudo ip link set can1 up
sudo ip link set can2 up
candump -H any                     # -H: the adapter's hardware timestamps
```

`scripts/triple-attach.sh ttyACM0` does all of that with these defaults
(it loads the module if needed); `scripts/triple-detach.sh` undoes it.
To detach a single adapter, kill its `ldattach`; unplugging works too.

| Interface | Connector | `dev_port` |
|---|---|---|
| `can0` | CAN1, classic CAN | 0 |
| `can1` | CAN2, classic CAN | 1 |
| `can2` | CAN3, CAN FD | 2 |

The numbers continue for a second adapter (`can3` .. `can5`); see
[stable names](#automatic-attach-and-stable-names).

A few everyday commands:

```sh
cansend can0 123#11223344                  # classic frame
cansend can2 456##10102030405060708090A0B0C   # CAN FD, flags 1 = bit rate switch
cangen can0 -g 10                          # random traffic every 10 ms
candump -e any,0:0,#FFFFFFFF               # error frames too
ip -details -s link show can0              # state, counters, statistics
```

## Bitrates

| Port | `bitrate` | `dbitrate` (`fd on`) | Options |
|---|---|---|---|
| CAN1, CAN2 | 10k 20k 33333 50k 62500 83333 100k 125k 250k 500k 1M | - | `listen-only on` |
| CAN3 | 125k 250k 500k 1M | 250k 500k 833333 1M 1.5M 2M 3M 4M 5M 6.7M 8M 10M, higher than `bitrate` | `listen-only on`, `fd on`, `fd-non-iso on` |

CAN3 always runs as CAN FD in the firmware. Without `fd on` the
interface sends and receives classic frames only, and the data bitrate
defaults to the next value above the nominal one.

## Any bit timing on CAN3

With the module parameter `fd_bittiming=1`, CAN3 takes any bitrate its
controller (MCP2517FD, 40 MHz clock) can reach, any sample point, and
bit timing in time quanta. The kernel computes the timing; the driver
sends it as raw controller timing. Transmitter delay compensation is
switched on automatically when the data prescaler is 1 or 2.

```sh
echo "options usb2cansocketcan fd_bittiming=1" | sudo tee /etc/modprobe.d/usb2cansocketcan.conf
sudo ip link set can2 type can bitrate 500000 sample-point 0.8 \
     dbitrate 2000000 dsample-point 0.8 fd on
sudo ip link set can2 type can bitrate 800000 sample-point 0.8 \
     dbitrate 4000000 dsample-point 0.8 fd on       # not in the fixed table
```

Give both sample points as 0.8 to talk to other Triples and to PP2CAN.
The firmware's fixed table uses about 80 %; with the kernel's defaults
(87.5 % nominal below 800 kbit/s, 75 % data) frames with bit rate
switching were lost in tests. `scripts/triple-attach.sh` adds them when
the parameter is set. `ip -details link show can2` shows the computed
timing, and the kernel log the raw values sent.

## Bus state, errors and statistics

The adapter reports error counters and state for each port every
250 ms. The driver maps them to SocketCAN:

* `ip -details link show` shows the state (`ERROR-ACTIVE`,
  `ERROR-WARNING`, `ERROR-PASSIVE`) and `berr-counter tx .. rx ..`.
* State changes are delivered as error frames (`CAN_ERR_CRTL`) with TEC
  and REC in data bytes 6 and 7.
* Frames the adapter lost toward the PC (receive overload) count in
  `rx_over_errors` and produce a `CAN_ERR_CRTL_RX_OVERFLOW` error frame.
* Frames the adapter dropped because its transmit buffer was full count
  in `tx_dropped` and produce a `CAN_ERR_CRTL_TX_OVERFLOW` error frame;
  with the flow control below this should not happen.

The CAN controller recovers from error passive on its own. Bus off has
not been observed on hardware; how the firmware reports it is not known
yet.

## Transmit flow control

The adapter takes frames from USB faster than any bus can send them and
drops what does not fit into its transmit buffer (220 frames on CAN1 and
CAN2, 120 classic or about 100 FD frames on CAN3). The driver keeps
each port within the buffer, using the buffer fill the adapter reports
every 250 ms: after each report it allows up to three quarters of the
buffer at once, plus three quarters of what the bus drained in the last
period, released evenly.

What that means in practice:

* On a slow or blocked bus (no other node acknowledging, wrong
  bitrate) the queue stops instead of losing frames: `cangen` blocks,
  `cansend` returns `No buffer space available` once the socket queue is
  full.
* A healthy 1 Mbit/s bus takes about 2100 frames/s from one port.
* Tested on firmware 1.41: 0 frames lost out of 2000 .. 10000 per run on
  a blocked CAN1, a blocked CAN3 with 8, 24 and 64-byte FD frames, a
  healthy CAN1 at 1 Mbit/s and a full 250 kbit/s bus. Details are in
  [protocol.md](protocol.md), section 5.1.

`debug=1` logs the buffer fill and the credit per status period.

## Automatic attach and stable names

`etc/udev/rules.d/90-usb2can-triple.rules` starts
`etc/systemd/system/usb2can-triple@.service` (which runs `ldattach`)
whenever an adapter is plugged in, and optionally names the interfaces
after the adapter's USB serial number and connector:

```sh
sudo cp etc/udev/rules.d/90-usb2can-triple.rules /etc/udev/rules.d/
sudo cp etc/systemd/system/usb2can-triple@.service /etc/systemd/system/
sudo udevadm control --reload && sudo systemctl daemon-reload
```

The three devices carry `dev_port` 0, 1, 2 (CAN1 .. CAN3) in sysfs, and
the serial number is an attribute of the parent USB device. Bitrates are
not set by the rule; set them with `ip link` or systemd-networkd
(`.network` files with a `[CAN]` section).

## WSL2

On Windows, an adapter can be handed to WSL2 with
[usbipd-win](https://github.com/dorssel/usbipd-win). The module has to be
built against the WSL2 kernel source (Microsoft's
`WSL2-Linux-Kernel` repository at the tag of `uname -r`), because WSL2
has no kernel headers package.

```sh
usbipd list                          # Windows, admin: find the busid of 04d8:f0d7
usbipd bind --busid 1-2              # once per adapter
usbipd attach --wsl --busid 1-2      # appears in WSL2 as /dev/ttyACMx
```

An adapter handed back to Windows (`usbipd detach`) may stay unusable
there (opening its COM port times out) until it is physically
replugged; `pnputil /restart-device` does not help. Receive rates
through usbipd are lower than on native USB.

## Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `fd_bittiming` | 0 | CAN3: any bitrate and sample point, bit timing computed by the kernel ([above](#any-bit-timing-on-can3)); set at load time |
| `hwtstamp` | 1 | ask the adapter for 1 µs receive timestamps; off raises the sustainable receive rate by about a third; set at load time |
| `max_wire_len` | 100 | drop frames longer than this many bytes on the wire (see Limits); writable at runtime |
| `debug` | 0 | hex dump every message exchanged with the adapter and log the flow control; writable at runtime |

Runtime values are in `/sys/module/usb2cansocketcan/parameters/`.

## Limits

* **100 bytes per message.** CAN FD frames with many data bytes equal
  to 0x0F, 0x1F or 0xEF need escaping and can exceed it; such frames are
  dropped (`tx_dropped`), because longer messages lock up the adapter's
  firmware until it is power cycled. `max_wire_len` raises the limit if a
  newer firmware allows it.
* **Receive rate** toward the PC is roughly 2000 to 5000 frames/s per
  adapter (measured through usbipd into WSL2); beyond that the firmware
  drops frames and the driver counts the overloads.
* CAN1 and CAN2 only take the firmware's fixed bitrates.
* The trigger/marker input and the firmware's bus-off indication are not
  decoded.

## Troubleshooting

| Symptom | Check |
|---|---|
| No `can0` after `ldattach` | `dmesg`: module loaded? `lsmod \| grep usb2cansocketcan`; is the port right (`ls /dev/ttyACM*`)? |
| `RTNETLINK answers: Invalid argument` on `ip link set ... bitrate` | The bitrate is not in the table for that port; on CAN3 use `fd_bittiming=1` for others. |
| Interface up but nothing received | Bitrate, termination, another node on the bus; `ip -details -s link show` shows the error counters. |
| `cansend` fails with "No buffer space available" | The bus does not take the frames (no acknowledging node, wrong bitrate) and the queue is full; see flow control. |
| Port busy | SavvyCAN or `triple_dump.py` has it open; the serial port can only be used by one program. |
| Hex dumps needed for a bug report | `echo 1 \| sudo tee /sys/module/usb2cansocketcan/parameters/debug`, then `dmesg`. |

## Without the kernel module

`tools/triple_dump.py` talks to an adapter directly over its serial port
with pyserial and prints received frames; it runs on Windows and macOS
as well:

```sh
python3 tools/triple_dump.py /dev/ttyACM0                  # print frames
python3 tools/triple_dump.py COM7 --send 1 123#11223344    # send on CAN1
python3 tools/triple_dump.py COM7 --status                 # also the status messages
```

The wire protocol is documented in [protocol.md](protocol.md);
`tools/triple_proto.py` is the Python codec it uses.
