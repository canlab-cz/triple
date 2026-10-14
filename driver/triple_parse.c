#include <linux/string.h>
#include <linux/module.h>

#include "triple_parse.h"

extern bool show_debug_pars;
extern bool trace_func_pars;
extern void print_func_trace(bool is_trace, int line, const char *func);

/*
 * Wire format (before escaping):
 *   [0] U2C_TR_FIRST_BYTE
 *   [1] total length of the escaped frame
 *   [2] command (U2C_TR_CMD_TX_CAN_x)
 *   [3..6] CAN identifier, big endian
 *   [7] flags | DLC code:  0x80 extended id, 0x40 RTR, 0x20 CAN FD,
 *                          0x10 bit rate switch, 0x0F DLC code
 *   [8..] data bytes
 *   [last] U2C_TR_LAST_BYTE
 * Any payload byte equal to FIRST/LAST/SPEC is prefixed by U2C_TR_SPEC_BYTE.
 */

int TripleSendHex(TRIPLE_CAN_FRAME *frame)
{
  unsigned char *p = frame->comm_buf;
  unsigned char dlc = 0;
  unsigned char cmd;
  int length = 0;
  int i;

  memset(p, 0, sizeof(frame->comm_buf));

  /* byte 0 - FIRST BYTE, byte 1 - length (patched at the end) */
  p[length++] = U2C_TR_FIRST_BYTE;
  p[length++] = 1;

  /* byte 2 - command: TX to CAN port */
  switch (frame->CAN_port)
  {
  case 1:  cmd = U2C_TR_CMD_TX_CAN_1; break;
  case 2:  cmd = U2C_TR_CMD_TX_CAN_2; break;
  case 3:  cmd = U2C_TR_CMD_TX_CAN_3; break;
  default: cmd = U2C_TR_CMD_TX_CAN;   break;
  }
  length += USB2CAN_TRIPLE_PushByte(cmd, p + length);

  /* ID, big endian */
  for (i = 0; i < ID_LEN; i++)
    length += USB2CAN_TRIPLE_PushByte(frame->id[i], p + length);

  /* flags + DLC code */
  USB2CAN_TRIPLE_CANFD_DLCFromLength(&dlc, frame->dlc);
  if (frame->id_type)
    dlc |= 0x80;
  if (frame->rtr)
    dlc |= 0x40;
  if (frame->fd)
  {
    dlc |= 0x20; /* CAN FD frame */
    if (frame->fd_br_switch)
      dlc |= 0x10;
  }
  length += USB2CAN_TRIPLE_PushByte(dlc, p + length);

  /* data */
  for (i = 0; i < frame->dlc; i++)
    length += USB2CAN_TRIPLE_PushByte(frame->data[i], p + length);

  /* LAST BYTE (never escaped) */
  length += USB2CAN_TRIPLE_PushByteClear(U2C_TR_LAST_BYTE, p + length);

  /* Total escaped length goes into byte 1. If that value is itself a
   * special byte it has to be escaped too, which shifts the rest of the
   * frame by one and makes the length one larger (0x10, 0x20 or 0xF0). */
  if (length == U2C_TR_FIRST_BYTE || length == U2C_TR_LAST_BYTE || length == U2C_TR_SPEC_BYTE)
  {
    memmove(p + 3, p + 2, length - 2);
    length++;
    p[1] = U2C_TR_SPEC_BYTE;
    p[2] = (unsigned char)length;
  }
  else
  {
    p[1] = (unsigned char)length;
  }
  return length;
}

int TripleRecvHex(TRIPLE_CAN_FRAME *frame, size_t len)
{
  const unsigned char *p = frame->comm_buf;
  const size_t offset = 2; /* position of the command byte */
  unsigned char cmd;
  unsigned char flags;
  size_t need;

  if (len < 4 || p[0] != U2C_TR_FIRST_BYTE || p[len - 1] != U2C_TR_LAST_BYTE)
    return -1;

  cmd = p[offset];

  if (cmd == U2C_TR_CMD_STATUS)
    return 1;

  if (cmd == U2C_TR_CMD_FW_VER)
    return 2;

  switch (cmd)
  {
  case U2C_TR_CMD_TX_CAN_1:
    frame->CAN_port = 0;
    break;
  case U2C_TR_CMD_TX_CAN_2:
    frame->CAN_port = 1;
    break;
  case U2C_TR_CMD_TX_CAN_3:
    frame->CAN_port = 2;
    break;
  case U2C_TR_CMD_TX_CAN_3_ESI:
    frame->CAN_port = 2;
    frame->fd_esi = true;
    break;
  default:
    /* timestamped RX (0xB0..0xB3), marker, counters: not decoded yet */
    if (show_debug_pars)
      printk(KERN_DEBUG "triple: unsupported command 0x%02x\n", cmd);
    return -2;
  }

  /* command + 4 id bytes + flags byte must be present */
  if (len < offset + 6 + 1)
    return -3;

  flags = p[offset + 5];

  if (flags & 0x80)
    frame->id_type = 1;

  if (flags & 0x20)
  {
    frame->fd = true;
    if (flags & 0x10)
      frame->fd_br_switch = true;
  }

  if (flags & 0x40)
    frame->rtr = 1;

  frame->dlc = USB2CAN_TRIPLE_CANFD_LengthFromDLC(flags & 0x0F);

  if (!frame->fd && frame->dlc > DATA_LEN)
    return -4;

  memcpy(frame->id, p + offset + 1, ID_LEN);

  if (!frame->rtr)
  {
    need = offset + 6 + frame->dlc + 1; /* + trailing LAST_BYTE */
    if (len < need)
      return -5;
    memcpy(frame->data, p + offset + 6, frame->dlc);
  }

  return 0;
}
