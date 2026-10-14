/* Host-side test of the USB2CAN Triple wire codec.
 *
 * Besides encode -> decode round trips it checks the codec against byte
 * sequences captured from firmware 1.41 on 2026-10-14, so a change that
 * breaks compatibility with the adapter fails here before touching hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "triple_proto.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void hexdump(const char *tag, const u8 *p, int n)
{
  int i;

  printf("%-34s %3d:", tag, n);
  for (i = 0; i < n && i < 26; i++)
    printf(" %02X", p[i]);
  printf("%s\n", n > 26 ? " ..." : "");
}

static int parse_hex(const char *s, u8 *out)
{
  int n = 0;
  unsigned v;

  while (sscanf(s, "%2x", &v) == 1)
  {
    out[n++] = (u8)v;
    s += 2;
    while (*s == ' ')
      s++;
  }
  return n;
}

/* The firmware echoes the TX layout back on receive (without timestamp), so a
 * TX message for port p decodes as an RX message for port p. */
static void roundtrip(const char *name, int port, int fd, int rtr, int eff, u32 id, int len, int special_data)
{
  struct triple_frame tx;
  struct triple_msg m;
  u8 wire[TRIPLE_MTU];
  int n, i, type;

  memset(&tx, 0, sizeof(tx));
  tx.port = port; tx.fd = fd; tx.rtr = rtr; tx.eff = eff; tx.id = id; tx.len = len;
  for (i = 0; i < len; i++)
    tx.data[i] = special_data ? ((i % 4 == 0) ? 0x0F : (i % 4 == 1) ? 0xEF : (i % 4 == 2) ? 0x1F : (u8)i) : (u8)(0x40 + i);

  n = triple_encode_frame(&tx, wire, sizeof(wire));
  hexdump(name, wire, n);
  CHECK(n > 0 && n <= TRIPLE_MTU);
  CHECK(wire[0] == TRIPLE_FIRST_BYTE && wire[n - 1] == TRIPLE_LAST_BYTE);
  CHECK(wire[1] == (u8)n);                      /* raw length, never escaped */
  for (i = 2; i < n - 1; i++)                   /* no bare end byte inside */
    if (wire[i] == TRIPLE_LAST_BYTE)
      CHECK(wire[i - 1] == TRIPLE_ESC_BYTE);

  type = triple_decode(wire, n, &m);
  CHECK(type == TRIPLE_MSG_FRAME);
  CHECK(m.frame.port == port);
  CHECK(m.frame.fd == fd);
  CHECK(m.frame.rtr == rtr);
  CHECK(m.frame.eff == eff);
  CHECK(m.frame.id == id);
  CHECK(m.frame.len == len);
  CHECK(!m.frame.has_ts);
  if (!rtr)
    CHECK(memcmp(m.frame.data, tx.data, len) == 0);
}

static void captured(void)
{
  struct triple_msg m;
  u8 wire[TRIPLE_MTU];
  u8 cmd[32];
  int n, type;

  printf("--- messages captured from firmware 1.41\n");

  /* timestamped standard frame on port 1 */
  n = parse_hex("0f 15 b0 00 00 01 23 08 00 29 35 82 11 22 33 44 55 66 77 88 ef", wire);
  type = triple_decode(wire, n, &m);
  CHECK(type == TRIPLE_MSG_FRAME && m.frame.port == 0 && !m.frame.eff && !m.frame.fd);
  CHECK(m.frame.id == 0x123 && m.frame.len == 8 && m.frame.has_ts && m.frame.ts == 0x00293582);
  CHECK(m.frame.data[0] == 0x11 && m.frame.data[7] == 0x88);

  /* timestamped extended frame, escaped data byte */
  n = parse_hex("0f 12 b0 1a bc de f0 84 00 2d d3 34 de ad be 1f ef ef", wire);
  type = triple_decode(wire, n, &m);
  CHECK(type == TRIPLE_MSG_FRAME && m.frame.eff && m.frame.id == 0x1ABCDEF0 && m.frame.len == 4);
  CHECK(m.frame.ts == 0x002dd334 && m.frame.data[3] == 0xEF);

  /* timestamped FD frame with BRS on port 3, dlc code 9 = 12 bytes */
  n = parse_hex("0f 19 b2 00 00 01 00 39 00 37 0d 15 00 01 02 03 04 05 06 07 08 09 0a 0b ef", wire);
  type = triple_decode(wire, n, &m);
  CHECK(type == TRIPLE_MSG_FRAME && m.frame.port == 2 && m.frame.fd && m.frame.brs && !m.frame.esi);
  CHECK(m.frame.id == 0x100 && m.frame.len == 12 && m.frame.data[11] == 0x0b);

  /* untimestamped frame on port 2, 15 bytes with raw length byte 0x0F */
  n = parse_hex("0f 0f a1 00 00 01 23 06 11 22 33 44 55 66 ef", wire);
  type = triple_decode(wire, n, &m);
  CHECK(type == TRIPLE_MSG_FRAME && m.frame.port == 1 && m.frame.len == 6 && !m.frame.has_ts);

  /* ESI variant of port 3 */
  n = parse_hex("0f 0a b3 00 00 00 10 20 00 00 00 01 ef", wire);
  type = triple_decode(wire, n, &m);
  CHECK(type == TRIPLE_MSG_FRAME && m.frame.port == 2 && m.frame.esi && m.frame.fd && m.frame.len == 0);

  /* firmware version */
  n = parse_hex("0f 08 90 31 2e 34 31 ef", wire);
  type = triple_decode(wire, n, &m);
  CHECK(type == TRIPLE_MSG_FW_VER && strcmp(m.fw_version, "1.41") == 0);

  /* status: port 1 TEC 0x80 REC 0x08 state 1 (sender without ACK) */
  n = parse_hex("0f 2a 8a 05 80 08 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 20 00 01 00 00 00 00 00 00 00 00 ef", wire);
  CHECK(n == 42);
  type = triple_decode(wire, n, &m);
  CHECK(type == TRIPLE_MSG_STATUS);
  CHECK(m.status.port[0].tec == 0x80 && m.status.port[0].rec == 0x08 && m.status.port[0].state == 1);
  CHECK(m.status.port[1].tec == 0 && m.status.port[1].state == 0);

  /* status: port 2 REC 0x80 state 1 (receiver at the wrong bitrate) */
  n = parse_hex("0f 2a 8a 05 00 00 00 80 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 20 00 00 01 00 00 00 00 00 00 00 ef", wire);
  type = triple_decode(wire, n, &m);
  CHECK(type == TRIPLE_MSG_STATUS && m.status.port[1].rec == 0x80 && m.status.port[1].state == 1);

  /* unsupported command (marker) is reported, not misdecoded */
  n = parse_hex("0f 05 87 01 ef", wire);
  CHECK(triple_decode(wire, n, &m) == -2);

  /* truncated frame */
  n = parse_hex("0f 09 a0 00 00 01 23 08 ef", wire);
  CHECK(triple_decode(wire, n, &m) == -5);

  printf("--- commands, compared with the bytes the probe sent to firmware 1.41\n");
  n = triple_encode_settings_can(cmd, 1, 250, false);
  CHECK(n == 8 && memcmp(cmd, "\x0f\x08\x88\x01\x00\xfa\x00\xef", 8) == 0);
  n = triple_encode_settings_fd(cmd, 1256700, false, false, false);
  CHECK(n == 12 && memcmp(cmd, "\x0f\x0c\x88\x03\x00\x13\x2c\xfc\x00\x00\x00\xef", 12) == 0);
  n = triple_encode_timestamp_mode(cmd, true);
  CHECK(n == 5 && memcmp(cmd, "\x0f\x05\x8b\x01\xef", 5) == 0);
  n = triple_encode_buffer_mode(cmd, 1);
  CHECK(n == 8 && memcmp(cmd, "\x0f\x08\x8d\x00\x00\x00\x01\xef", 8) == 0);
  n = triple_encode_fw_version_req(cmd);
  CHECK(n == 4 && memcmp(cmd, "\x0f\x04\x90\xef", 4) == 0);

  printf("--- bitrate tables\n");
  CHECK(triple_can_speed_kbit(250000) == 250 && triple_can_speed_kbit(33333) == 33 && triple_can_speed_kbit(300000) == 0);
  CHECK(triple_fd_speed_code(125000, 6700000) == 1256700);
  CHECK(triple_fd_speed_code(125000, 250000) == 125250);
  CHECK(triple_fd_speed_code(250000, 1000000) == 2501000);
  CHECK(triple_fd_speed_code(500000, 833333) == 500833);
  CHECK(triple_fd_speed_code(1000000, 1500000) == 10001500);
  CHECK(triple_fd_speed_code(125000, 10000000) == 1259999);
  CHECK(triple_fd_speed_code(500000, 250000) == 0);
  CHECK(triple_fd_speed_code(300000, 1000000) == 0);
  CHECK(triple_fd_default_data_bitrate(125000) == 250000 && triple_fd_default_data_bitrate(1000000) == 1500000);
}

int main(void)
{
  struct triple_frame f;
  u8 wire[TRIPLE_MTU];

  printf("--- round trips\n");
  roundtrip("can2.0 std dlc8 specials", 0, 0, 0, 0, 0x123, 8, 1);
  roundtrip("can2.0 ext dlc0", 1, 0, 0, 1, 0x1ABCDEF0, 0, 0);
  roundtrip("can2.0 std rtr dlc4", 0, 0, 1, 0, 0x7FF, 4, 0);
  roundtrip("can2.0 std dlc6 (15 bytes, raw 0F)", 0, 0, 0, 0, 0x123, 6, 0);
  roundtrip("can2.0 ext dlc8 1F id (31 bytes?)", 1, 0, 0, 1, 0x1F1F1F1F, 8, 0);
  roundtrip("canfd std dlc12", 2, 1, 0, 0, 0x100, 12, 0);
  roundtrip("canfd ext dlc12 specials (31 B)", 2, 1, 0, 1, 0x1FFFFFFF, 12, 1);
  roundtrip("canfd std dlc64 plain (73 B)", 2, 1, 0, 0, 0x100, 64, 0);
  roundtrip("canfd std dlc64 specials (137 B)", 2, 1, 0, 0, 0x0F, 64, 1);

  /* limits */
  memset(&f, 0, sizeof(f));
  f.port = 0; f.len = 9;
  CHECK(triple_encode_frame(&f, wire, sizeof(wire)) < 0);   /* 9 bytes on CAN 2.0 */
  f.port = 3; f.len = 1;
  CHECK(triple_encode_frame(&f, wire, sizeof(wire)) < 0);   /* no such port */
  f.port = 2; f.fd = true; f.len = 64;
  CHECK(triple_encode_frame(&f, wire, 72) < 0);              /* output too small */

  captured();

  printf("%s (%d failures)\n", fails ? "CODEC TEST FAILED" : "CODEC TEST PASSED", fails);
  return fails ? 1 : 0;
}
