# PP2CAN features versus SavvyCAN with the Triple connection

Phase 4 builds the analyzer on SavvyCAN instead of re-implementing PP2CAN.
This table maps every PP2CAN feature (from the CANLAB introduction page,
canlab.cz/cs/pp2can_seznameni) to what SavvyCAN already offers, so the
remaining work targets real gaps only.

Reviewed 2026-10-14 against SavvyCAN branch QT6WIP (3288f1f) with the
USB2CAN Triple connection from `gui/savvycan/`.

Legend: **yes** = covered, **partial** = usable with a different workflow
or missing details, **no** = not available.

## Logging and viewing

| PP2CAN | SavvyCAN | Status | Notes |
|---|---|---|---|
| Received message log | main frame list | yes | Timestamps from the adapter (1 us), bus column, filters by bus / ID / direction / length |
| Sent message log | same list, Dir = Tx | yes | Direction filter separates Tx and Rx |
| Disable R log / S log | Suspend Capturing, direction filter | yes | |
| Grid log | Overwrite Mode | yes | One row per ID with count and time delta |
| Filtered log, Log filter, Msg. filter | Frame Filtering, Save/Load Filter Definition | yes | |
| Find | Frame Search | yes | |
| File logging (long captures) | Start Continuous Logging | yes | Many formats (CRTD, GVRET CSV, candump, BLF, Vector ASC, ...) |
| F1..F8 markers in the log | none | no | PP2CAN also logs the adapter's trigger input; needs the marker message decoded (firmware source) |
| Save log / Print | Save Log File, Save Decoded Frames (CSV) | partial | No printing |
| Information window | Connection window console | yes | Firmware version, settings, error counters |

## Sending

| PP2CAN | SavvyCAN | Status | Notes |
|---|---|---|---|
| Manual message block | quick-send grid in the main window | yes | Numbers need a `0x` prefix, see `gui/savvycan/README.md` |
| Predefined message database, up to 10 selections | Custom frame sender (Send Frames > Custom), save/load | yes | |
| Sync (periodic 10 ms .. 10 s) | Custom frame sender intervals | yes | Host timed |
| HW sync (adapter-timed periodic) | none | no | Needs the firmware's periodic-transmit command; not in the documented protocol |
| File sender (replay a log) | Playback | yes | Speed control, loop, per-bus mapping |
| Multipacket sender | ISO-TP via scripting | partial | No generic multi-frame sender window |
| Data sender (byte / 16-bit value with a slider) | none | no | Scripting can do it, no slider UI |
| Bit sender | none | no | |
| ASCII sender | none | no | ASCII column only on receive |
| Signal sender | DBC + frame sender | partial | Signals are edited in the DBC editor, not with sliders |
| Reply maker | Custom frame sender triggers, Scripting | partial | Triggers ("on ID x after n ms") and modifiers exist; JavaScript scripting covers data-dependent replies |
| Gateway | CAN Bridge | yes | Between any two buses, also across adapters |
| Fault injector | Fuzzing (random frames) | partial | No physical-layer fault injection |

## Analysis

| PP2CAN | SavvyCAN | Status | Notes |
|---|---|---|---|
| Data receiver (byte / 16-bit value with bar) | Signal Viewer with a DBC | partial | Needs a DBC signal definition |
| Bit receiver with graph | Sniffer, Single/Multi State, graphing | yes | |
| ASCII receiver | ASCII column | partial | No text log window |
| Signal receiver | DBC decoding (Interpret Frames), Signal Viewer | yes | |
| Graph | Graphing windows, Temporal Graph | yes | |
| ID trap (IDs and counts on the bus) | Frame Data Analysis, Overwrite Mode | yes | |
| Timing accuracy (period per ID) | Frame Data Analysis | yes | Shows interval statistics per ID |
| Response meter | none | no | Time from request ID to response ID |
| Bus load graph with BusOff, TEC/REC, overflow counts | Frames Per Second only | partial | TEC/REC and state changes are now error frames in the log; no load % and no counter graph |
| Table reader, Data view | Frame Info, Flow View | yes | |
| Set / Get number | DBC signal editor | partial | |
| CAN calculator (29-bit, 11+18, MCP2510, SJA1000 registers) | none | no | Small standalone utility |

## Protocols and system

| PP2CAN | SavvyCAN | Status | Notes |
|---|---|---|---|
| CANopen tools, CANopen Safety, CANopen FD, dictionary | none | no | Largest gap. SavvyCAN has J1939, ISO-TP and UDS |
| Scripting | JavaScript (QJSEngine) Scripting Interface | yes | Different language than PP2CAN scripts |
| Plugins | none, but open source | partial | New windows are added in C++ |
| Listen only | per bus in the connection window | yes | |
| Loopback | none | no | Adapter support unknown |
| Reset CAN / Reset port | Reset Selected Device | yes | |
| Speed settings | per bus, incl. CAN FD data rate on CAN3 | yes | |
| Windows | Windows build verified | yes | Also Linux and macOS |

## What to build next

Ranked by value for CANLAB users and by how much is unblocked today.

1. **Bus statistics window.** Bus load in percent per bus (from frame
   lengths and the configured bitrate), TEC/REC and state over time from
   the adapter's status messages, error and overflow counts. Covers
   PP2CAN's Bus load tool and uses data the connection already decodes.
   Self-contained SavvyCAN window, no upstream changes elsewhere.
2. **CANopen basics.** NMT master commands, heartbeat / node monitor, SDO
   client (expedited and segmented upload/download), PDO decoding from an
   EDS file. The biggest functional gap; a sizeable but well specified
   job (CiA 301). Could live upstream as a bus protocol next to J1939.
3. **Response meter.** Request ID, response ID, statistics of the delay.
   Small window on top of the frame stream.
4. **Data / bit / ASCII sender with sliders.** A "panel" window that
   edits one frame's bytes, bits or a 16-bit value live with sliders and
   checkboxes. Useful for bench testing, small.
5. **CAN calculator.** ID conversions between 29-bit, 11+18 and the
   MCP2510 / SJA1000 register layouts. Trivial utility.

Blocked on the firmware source (see `doc/protocol.md`):

* **HW sync**: adapter-timed periodic transmit needs its command layout.
* **Markers**: trigger input events (`0x87`) and F1..F8 markers in the log.
* **Overflow counter**: which status byte counts dropped frames
  (byte 14 looks like a buffer fill level).
* **Bus off**: the encoding of bus off in the status message.
