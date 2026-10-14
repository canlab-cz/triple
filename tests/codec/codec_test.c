#include <linux/module.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "triple_parse.h"

bool show_debug_pars = true;
bool trace_func_pars = false;
void print_func_trace(bool is_trace, int line, const char *func) {}

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* The firmware echoes TX commands back with the same layout, so a TX frame
 * for port N is a valid RX frame for port N-1 (0-based). */
static void roundtrip(const char *name, int port, int fd, int rtr, int eff, unsigned id, int dlc)
{
  TRIPLE_CAN_FRAME tx, rx;
  int len, i, ret;
  size_t ulen;

  memset(&tx, 0, sizeof(tx));
  tx.CAN_port = port; tx.fd = fd; tx.rtr = rtr; tx.id_type = eff; tx.dlc = dlc;
  for (i = ID_LEN - 1; i >= 0; i--) { tx.id[i] = id & 0xff; id >>= 8; }
  /* data deliberately includes every special byte */
  for (i = 0; i < dlc; i++) tx.data[i] = (i % 4 == 0) ? U2C_TR_FIRST_BYTE : (i % 4 == 1) ? U2C_TR_LAST_BYTE : (i % 4 == 2) ? U2C_TR_SPEC_BYTE : (unsigned char)i;

  len = TripleSendHex(&tx);
  printf("%-28s wire len %3d:", name, len);
  for (i = 0; i < len && i < 24; i++) printf(" %02X", tx.comm_buf[i]);
  printf("%s\n", len > 24 ? " ..." : "");

  CHECK(len <= COM_BUF_LEN);
  CHECK(tx.comm_buf[0] == U2C_TR_FIRST_BYTE);
  CHECK(tx.comm_buf[len - 1] == U2C_TR_LAST_BYTE);
  /* the length byte is the raw wire length, never escaped */
  CHECK(tx.comm_buf[1] == (unsigned char)len);
  /* no unescaped terminator before the end */
  for (i = 2; i < len - 1; i++)
    if (tx.comm_buf[i] == U2C_TR_LAST_BYTE) CHECK(tx.comm_buf[i - 1] == U2C_TR_SPEC_BYTE);

  memset(&rx, 0, sizeof(rx));
  ulen = triple_unescape_frame(rx.comm_buf, tx.comm_buf, len);
  ret = TripleRecvHex(&rx, ulen);
  CHECK(ret == 0);
  CHECK(rx.CAN_port == port - 1);
  CHECK(rx.fd == fd);
  CHECK(rx.rtr == rtr);
  CHECK(rx.id_type == eff);
  CHECK(rx.dlc == dlc);
  CHECK(memcmp(rx.id, tx.id, ID_LEN) == 0);
  if (!rtr) CHECK(memcmp(rx.data, tx.data, dlc) == 0);
}

static void roundtrip_plain(const char *name, int port, int fd, int rtr, int eff, unsigned id, int dlc)
{
  TRIPLE_CAN_FRAME tx, rx;
  int len, i, ret;
  size_t ulen;

  memset(&tx, 0, sizeof(tx));
  tx.CAN_port = port; tx.fd = fd; tx.rtr = rtr; tx.id_type = eff; tx.dlc = dlc;
  for (i = ID_LEN - 1; i >= 0; i--) { tx.id[i] = id & 0xff; id >>= 8; }
  for (i = 0; i < dlc; i++) tx.data[i] = 0x40 + i;

  len = TripleSendHex(&tx);
  printf("%-28s wire len %3d:", name, len);
  for (i = 0; i < len && i < 24; i++) printf(" %02X", tx.comm_buf[i]);
  printf("%s\n", len > 24 ? " ..." : "");

  CHECK(tx.comm_buf[0] == U2C_TR_FIRST_BYTE);
  CHECK(tx.comm_buf[len - 1] == U2C_TR_LAST_BYTE);
  CHECK(tx.comm_buf[1] == (unsigned char)len);

  memset(&rx, 0, sizeof(rx));
  ulen = triple_unescape_frame(rx.comm_buf, tx.comm_buf, len);
  ret = TripleRecvHex(&rx, ulen);
  CHECK(ret == 0);
  CHECK(rx.CAN_port == port - 1);
  CHECK(rx.fd == fd);
  CHECK(rx.id_type == eff);
  CHECK(rx.dlc == dlc);
  CHECK(memcmp(rx.id, tx.id, ID_LEN) == 0);
  CHECK(memcmp(rx.data, tx.data, dlc) == 0);
}

int main(void)
{
  TRIPLE_CAN_FRAME f;
  unsigned char buf[8];
  int ret;

  roundtrip("can2.0 std dlc8", 1, 0, 0, 0, 0x123, 8);
  roundtrip("can2.0 ext dlc0", 2, 0, 0, 1, 0x1ABCDEF0, 0);
  roundtrip("can2.0 std rtr dlc4", 1, 0, 1, 0, 0x7FF, 4);
  roundtrip("canfd std dlc64", 3, 1, 0, 0, 0x100, 64);
  roundtrip("canfd ext dlc12", 3, 1, 0, 1, 0x1FFFFFFF, 12);
  roundtrip("canfd std dlc48 (0F/EF/1F)", 3, 1, 0, 0, 0x0F, 48);
  /* escaped total length lands exactly on a special byte: 15 and 31 */
  roundtrip_plain("can2.0 std dlc6 (len 15 raw)", 1, 0, 0, 0, 0x123, 6);
  roundtrip("canfd dlc20 + 2 esc (len 31 raw)", 3, 1, 0, 0, 0x100, 20);
  roundtrip_plain("can2.0 ext dlc8 + 1F id (len 31?)", 2, 0, 0, 1, 0x1F1F1F1F, 8);
  roundtrip_plain("canfd std dlc20 (len 31)", 3, 1, 0, 0, 0x100, 20);

  /* malformed input must be rejected, never read out of bounds */
  memset(&f, 0, sizeof(f));
  f.comm_buf[0] = U2C_TR_FIRST_BYTE; f.comm_buf[1] = 4; f.comm_buf[2] = U2C_TR_CMD_TX_CAN_1; f.comm_buf[3] = U2C_TR_LAST_BYTE;
  CHECK(TripleRecvHex(&f, 4) < 0);                       /* no id/flags */
  memset(&f, 0, sizeof(f));
  f.comm_buf[0] = U2C_TR_FIRST_BYTE; f.comm_buf[2] = U2C_TR_CMD_TX_CAN_1; f.comm_buf[7] = 0x08; f.comm_buf[8] = U2C_TR_LAST_BYTE;
  CHECK(TripleRecvHex(&f, 9) == -5);                     /* dlc 8 but no data */
  memset(&f, 0, sizeof(f));
  f.comm_buf[0] = U2C_TR_FIRST_BYTE; f.comm_buf[2] = U2C_TR_CMD_TX_CAN_1; f.comm_buf[7] = 0x0F; f.comm_buf[72] = U2C_TR_LAST_BYTE;
  CHECK(TripleRecvHex(&f, 73) == -4);                    /* dlc code 15 on a non-FD frame */
  memset(&f, 0, sizeof(f));
  f.comm_buf[0] = U2C_TR_FIRST_BYTE; f.comm_buf[2] = U2C_TR_CMD_TX_CAN_1_TS; f.comm_buf[3] = U2C_TR_LAST_BYTE;
  CHECK(TripleRecvHex(&f, 4) == -2);                     /* timestamped frame: unsupported, not port 0 */
  memset(&f, 0, sizeof(f));
  f.comm_buf[0] = U2C_TR_FIRST_BYTE; f.comm_buf[2] = U2C_TR_CMD_STATUS; f.comm_buf[3] = U2C_TR_LAST_BYTE;
  CHECK(TripleRecvHex(&f, 4) == 1);
  CHECK(TripleRecvHex(&f, 0) < 0);

  /* escape_memcpy: 1F 1F EF -> 1F EF (escaped escape, then terminator) */
  { unsigned char in[3] = { 0x1F, 0x1F, 0xEF }; size_t n = escape_memcpy(buf, in, 3); CHECK(n == 2 && buf[0] == 0x1F && buf[1] == 0xEF); }
  { unsigned char in[2] = { 0x1F, 0x0F }; size_t n = escape_memcpy(buf, in, 2); CHECK(n == 1 && buf[0] == 0x0F); }

  ret = fails ? 1 : 0;
  printf("%s (%d failures)\n", fails ? "CODEC TEST FAILED" : "CODEC TEST PASSED", fails);
  return ret;
}
