/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 CANLAB s.r.o. */
/*
 * USB2CAN Triple (CANLAB) wire protocol: constants, frame codec and
 * command encoders. Kernel and host builds (tests/codec) share this file.
 *
 * Wire format, every message:
 *   [0]    0x0F                  start byte
 *   [1]    length                total bytes on the wire, escapes included,
 *                                written RAW (never escaped)
 *   [2]    command
 *   [3..]  payload               bytes 0x0F / 0x1F / 0xEF are prefixed by 0x1F
 *   [last] 0xEF                  end byte
 *
 * CAN frame payload (commands 0xA0..0xA3 / 0xB0..0xB3):
 *   id[4] big endian, flags, [ts[4] big endian, 1 us ticks], data[]
 *   flags: 0x80 extended id, 0x40 RTR, 0x20 CAN FD, 0x10 bit rate switch,
 *          0x0F DLC code (CAN FD encoding)
 *
 * All of the above was verified against firmware 1.41 on 2026-10-14,
 * see doc/PLAN.md.
 */
#ifndef TRIPLE_PROTO_H
#define TRIPLE_PROTO_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TRIPLE_FIRST_BYTE   0x0F
#define TRIPLE_LAST_BYTE    0xEF
#define TRIPLE_ESC_BYTE     0x1F

/* host -> adapter */
#define TRIPLE_CMD_TX_CAN_1       0xA0
#define TRIPLE_CMD_TX_CAN_2       0xA1
#define TRIPLE_CMD_TX_CAN_3       0xA2
#define TRIPLE_CMD_SETTINGS       0x88  /* bitrate / mode per port */
#define TRIPLE_CMD_BITTIMING      0x89  /* raw MCP2517FD bit timing, port 3 */
#define TRIPLE_CMD_TIMESTAMP      0x8B  /* 1 byte: 0 off, 1 on */
#define TRIPLE_CMD_BUFFER_MODE    0x8D  /* u32 big endian, 1 */
#define TRIPLE_CMD_FW_VER         0x90  /* no payload, reply has the same command */

/* adapter -> host */
#define TRIPLE_CMD_RX_CAN_1       0xA0  /* no timestamp */
#define TRIPLE_CMD_RX_CAN_2       0xA1
#define TRIPLE_CMD_RX_CAN_3       0xA2
#define TRIPLE_CMD_RX_CAN_3_ESI   0xA3
#define TRIPLE_CMD_RX_TS_CAN_1    0xB0  /* with timestamp */
#define TRIPLE_CMD_RX_TS_CAN_2    0xB1
#define TRIPLE_CMD_RX_TS_CAN_3    0xB2
#define TRIPLE_CMD_RX_TS_CAN_3_ESI 0xB3
#define TRIPLE_CMD_STATUS         0x8A  /* every 250 ms, 38 payload bytes */
#define TRIPLE_CMD_MARKER         0x87  /* trigger input, not decoded */

#define TRIPLE_FLAG_EFF   0x80
#define TRIPLE_FLAG_RTR   0x40
#define TRIPLE_FLAG_FD    0x20
#define TRIPLE_FLAG_BRS   0x10
#define TRIPLE_FLAG_DLC   0x0F

#define TRIPLE_ID_LEN     4
#define TRIPLE_TS_LEN     4
#define TRIPLE_MAX_DATA   64
#define TRIPLE_PORTS      3

/* Longest message either side can produce: 1 + 1 + 2 + 8 + 2 + 8 + 128 + 1 */
#define TRIPLE_MTU        160

/* Largest message the firmware is known to accept from the host. 137 bytes
 * locked the adapter up until power cycle; 100 is the largest verified. */
#define TRIPLE_FW_MAX_WIRE_LEN 100

#define TRIPLE_STATUS_LEN 38

enum triple_msg_type {
  TRIPLE_MSG_FRAME = 1,
  TRIPLE_MSG_STATUS,
  TRIPLE_MSG_FW_VER,
};

struct triple_frame {
  u8   port;              /* 0..2 */
  bool eff;
  bool rtr;
  bool fd;
  bool brs;
  bool esi;
  bool has_ts;
  u32  id;                /* without flags */
  u32  ts;                /* adapter microseconds, 32 bit, wraps */
  u8   len;               /* data length in bytes */
  u8   data[TRIPLE_MAX_DATA];
};

/* Error counters per port as reported by the status message. Port 3 has a
 * wider block in the firmware status; only its first two bytes are mapped. */
struct triple_port_status {
  u8 tec;
  u8 rec;
  u8 state;               /* 0 active, 1 passive seen; others unknown */
  u8 tx_fill;             /* frames waiting in the adapter's transmit buffer */
  u8 tx_overflow;         /* frames from the host dropped because that buffer was
                             full, counter modulo 256 since power-up */
  bool rx_overload;       /* CAN1/CAN2: receive overload during the last period,
                             frames may have been lost (CAN3 never set it in tests) */
};

/* Transmit buffer size per port, as reported in tx_fill when full. */
#define TRIPLE_TX_BUFFER_CAN   220
#define TRIPLE_TX_BUFFER_FD    120

struct triple_status {
  struct triple_port_status port[TRIPLE_PORTS];
  u16 can3_frames;        /* frames on CAN3 (received or sent) in the last period */
  u8 raw[TRIPLE_STATUS_LEN];
};

struct triple_msg {
  enum triple_msg_type type;
  struct triple_frame frame;      /* TRIPLE_MSG_FRAME */
  struct triple_status status;    /* TRIPLE_MSG_STATUS */
  char fw_version[16];            /* TRIPLE_MSG_FW_VER, NUL terminated */
};

/* ---- escaping -------------------------------------------------------- */

static inline bool triple_needs_escape(u8 b)
{
  return b == TRIPLE_FIRST_BYTE || b == TRIPLE_LAST_BYTE || b == TRIPLE_ESC_BYTE;
}

/* Append one payload byte, escaped if needed. Returns bytes written (1 or 2). */
static inline int triple_put(u8 *out, u8 b)
{
  if (triple_needs_escape(b))
  {
    out[0] = TRIPLE_ESC_BYTE;
    out[1] = b;
    return 2;
  }
  out[0] = b;
  return 1;
}

/* Unescape a complete wire message of n bytes into dest (same size is
 * enough). Bytes 0 and 1 are copied raw. Returns the unescaped length. */
size_t triple_unescape(u8 *dest, const u8 *src, size_t n);

/* ---- frame codec ----------------------------------------------------- */

/* Encode a CAN frame for transmission. Returns the wire length, or a
 * negative value if the frame does not fit into out (outsz bytes). */
int triple_encode_frame(const struct triple_frame *f, u8 *out, size_t outsz);

/* Decode a complete, still escaped wire message. Returns the message type
 * (> 0), or a negative value for malformed or unsupported messages. */
int triple_decode(const u8 *wire, size_t n, struct triple_msg *m);

/* ---- command encoders (return wire length) --------------------------- */

/* Ports 1 and 2: bitrate in kbit/s as the firmware code (see
 * triple_can_speed_kbit), listen-only flag. port is 1 or 2. */
int triple_encode_settings_can(u8 *out, u8 port, u16 kbit, bool listen_only);

/* Port 3 (CAN FD): combined speed code (see triple_fd_speed_code). */
int triple_encode_settings_fd(u8 *out, u32 code, bool listen_only, bool iso, bool esi);

/* Port 3 raw MCP2517FD bit timing (TRIPLE_CMD_BITTIMING). The values are
 * the controller's register fields, i.e. each count minus one (0 = divide
 * by 1), at a 40 MHz system clock. Verified on the bus against the speed
 * code settings: 500k/2M, 250k/1M, 500k/5M (doc/protocol.md 4.2). */
#define TRIPLE_FD_CLOCK_HZ 40000000u

struct triple_fd_bittiming {
  u16 nbrp, ntseg1, ntseg2, nsjw;     /* nominal */
  u16 dbrp, dtseg1, dtseg2, dsjw;     /* data */
  u16 tdco, tdcv, tdcmod;             /* transmitter delay compensation, tdcmod 0 off 1 manual 2 auto;
                                         tdco is signed, in clock cycles */
};
int triple_encode_bittiming_fd(u8 *out, const struct triple_fd_bittiming *bt, bool listen_only, bool iso, bool esi);

/* Register values from a bit timing in time quanta, as the Linux CAN core
 * or a bit timing calculator gives it: prescaler, tseg1 (= propagation +
 * phase segment 1), tseg2 (phase segment 2), sjw (0 is taken as 1).
 * Transmitter delay compensation is automatic when the data prescaler is
 * 1 or 2, with the offset at the data sample point in clock cycles (at
 * most 63), and off otherwise. Returns 0, or -1 if a value is outside the
 * controller's range (nominal brp 1..256, tseg1 2..256, tseg2 and sjw
 * 1..128; data brp 1..256, tseg1 1..32, tseg2 and sjw 1..16). */
int triple_fd_bittiming_from_tq(struct triple_fd_bittiming *bt,
                                u32 nbrp, u32 ntseg1, u32 ntseg2, u32 nsjw,
                                u32 dbrp, u32 dtseg1, u32 dtseg2, u32 dsjw);

/* Data phase timing in time quanta for a bitrate, sample point about
 * 80 %, for when the interface is not in FD mode (the controller always
 * runs CAN FD). Returns the bitrate reached, or 0 if none is possible. */
u32 triple_fd_data_timing(u32 bitrate, u32 *brp, u32 *tseg1, u32 *tseg2, u32 *sjw);

/* Same for either phase (data selects the data phase limits). */
u32 triple_fd_timing(u32 bitrate, bool data, u32 *brp, u32 *tseg1, u32 *tseg2, u32 *sjw);

/* Raw timing for a nominal/data pair at 80 % sample points (the firmware
 * table's), for rates the speed codes of 4.1 do not cover. Returns 0, or
 * -1 if a rate cannot be reached within 1 %. */
int triple_fd_raw_for_rates(struct triple_fd_bittiming *bt, u32 nominal, u32 data);

int triple_encode_timestamp_mode(u8 *out, bool on);
int triple_encode_buffer_mode(u8 *out, u32 mode);
int triple_encode_fw_version_req(u8 *out);

/* ---- bitrate tables -------------------------------------------------- */

/* Firmware kbit code for a CAN 2.0 bitrate in bit/s, or 0 if unsupported. */
u16 triple_can_speed_kbit(u32 bitrate);

/* Firmware speed code for a CAN FD nominal/data bitrate pair in bit/s,
 * or 0 if the combination is not supported. */
u32 triple_fd_speed_code(u32 nominal, u32 data);

/* Default data bitrate to use with a nominal bitrate when the user did not
 * ask for CAN FD (the port always runs in FD mode in the firmware). */
u32 triple_fd_default_data_bitrate(u32 nominal);

/* ---- timestamps ------------------------------------------------------ */

/* Extends the adapter's 32-bit microsecond counter (wraps every 71.6 min)
 * to 64 bits. Frames from different ports can arrive slightly out of
 * timestamp order, so a frame up to TRIPLE_TS_REORDER_US older than the
 * newest one seen is taken as reordered, not as a counter wrap. */
#define TRIPLE_TS_REORDER_US 1000000u

struct triple_ts {
  u64  base;      /* added to the 32-bit counter */
  u32  last;      /* newest counter value seen */
  bool valid;
};

u64 triple_ts_extend(struct triple_ts *t, u32 ts);

/* DLC code <-> length */
u8 triple_dlc2len(u8 dlc);
u8 triple_len2dlc(u8 len);

#ifdef __cplusplus
}
#endif

#endif /* TRIPLE_PROTO_H */
