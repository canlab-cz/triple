# SavvyCAN with the USB2CAN Triple

[SavvyCAN](https://github.com/collin80/SavvyCAN) is an MIT-licensed Qt
CAN bus analyzer (multi-bus log, DBC decoding, graphing, replay, frame
sender with scripting, ISO-TP / UDS tools). This directory adds a native
connection type for the CANLAB USB2CAN Triple, talking to the adapter over
its serial port with the shared protocol codec, on Linux, Windows and
macOS, without the kernel module.

What the connection provides:

* three buses per adapter (CAN1, CAN2 classic, CAN3 CAN FD), bitrates,
  listen-only, FD data rate and non-ISO set from SavvyCAN's connection
  window (Bus Details tabs 0, 1, 2);
* receive with the adapter's 1 us hardware timestamps, transmit on all
  three buses, CAN FD with BRS on bus 2;
* adapter error counters and error state in the device console;
* the adapter's 100-byte message limit enforced on transmit.

## Apply to an upstream checkout

```sh
git clone https://github.com/collin80/SavvyCAN
python3 gui/savvycan/apply.py SavvyCAN     # copies the sources, edits 8 upstream files
```

`apply.py` is idempotent. It was written against upstream commit
`e439670` (October 2026); `savvycan-upstream.patch` is the same change as
a plain diff for reference or for a pull request upstream.

Files added to `SavvyCAN/connections/`:

| File | Content |
|---|---|
| `triple_serial.h/.cpp` | the `CANConnection` implementation (`CANCon::TRIPLE`) |
| `triple_proto.h/.c` | the protocol codec, copied from `driver/` |

## Build

Linux / WSL2 (Ubuntu 22.04):

```sh
sudo apt install qtbase5-dev qt5-qmake libqt5serialbus5-dev libqt5serialport5-dev \
     qtdeclarative5-dev qttools5-dev libqt5websockets5-dev libqt5opengl5-dev
mkdir SavvyCAN/build && cd SavvyCAN/build
qmake ../SavvyCAN.pro CONFIG+=release && make -j$(nproc)
./SavvyCAN
```

Windows: install Qt 5.15 with the SerialBus and SerialPort modules and
build `SavvyCAN.pro` from Qt Creator or with `qmake && nmake`/`mingw32-make`
exactly as upstream describes; the adapter appears as `COMx`.

## Use

Connection > Open Connection Window > Add New Device Connection >
"CANLAB USB2CAN Triple (serial)". The port list puts Triple adapters first.
After "Create New Connection" the device shows three buses; select a bus
tab under Bus Details to change bitrate, listen only or (bus 2) CAN FD and
data rate, then "Save Bus Settings". Enable Console shows every message
exchanged with the adapter, the firmware version and the error counters.

Defaults at connect: CAN1 250 kbit/s, CAN2 500 kbit/s, CAN3 FD 125 kbit/s
nominal, 6.7 Mbit/s data, ISO.

Verified on 2026-10-14 against the Linux kernel driver on a second adapter:
frames on all three buses, FD, timestamps, with SavvyCAN running under
WSLg.

## Limits

* The serial port cannot be shared with the kernel driver; detach the line
  discipline first (`pkill ldattach`).
* CAN FD frames are accepted on bus 2 only; frames longer than 100 bytes on
  the wire are dropped with a console message (adapter firmware limit).
* The adapter's receive side saturates at roughly 2000 to 5000 frames/s
  per adapter; the firmware drops silently beyond that.
