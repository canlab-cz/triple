// SPDX-License-Identifier: MIT
// Copyright (c) 2026 CANLAB s.r.o.
/*
 * USB2CAN Triple wire protocol codec. No kernel dependencies beyond
 * string.h so that tests/codec can compile it on the host.
 */
#include "triple_proto.h"

size_t triple_unescape(u8 *dest, const u8 *src, size_t n)
{
  size_t i;
  size_t out = 0;
  bool esc = false;

  for (i = 0; i < n; i++)
  {
    /* start byte and raw length byte are never escaped */
    if (i < 2)
    {
      dest[out++] = src[i];
      continue;
    }
    if (src[i] == TRIPLE_ESC_BYTE && !esc)
    {
      esc = true;
      continue;
    }
    dest[out++] = src[i];
    esc = false;
  }
  return out;
}

u8 triple_dlc2len(u8 dlc)
{
  static const u8 table[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };

  return table[dlc & TRIPLE_FLAG_DLC];
}

u8 triple_len2dlc(u8 len)
{
  if (len <= 8)
    return len;
  if (len <= 12)
    return 9;
  if (len <= 16)
    return 10;
  if (len <= 20)
    return 11;
  if (len <= 24)
    return 12;
  if (len <= 32)
    return 13;
  if (len <= 48)
    return 14;
  return 15;
}

/* ---- generic message builder ----------------------------------------- */

static int triple_build(u8 *out, u8 cmd, const u8 *payload, int n)
{
  int len = 0;
  int i;

  out[len++] = TRIPLE_FIRST_BYTE;
  out[len++] = 0; /* patched below */
  len += triple_put(out + len, cmd);
  for (i = 0; i < n; i++)
    len += triple_put(out + len, payload[i]);
  out[len++] = TRIPLE_LAST_BYTE;
  out[1] = (u8)len; /* raw, see header */
  return len;
}

/* ---- frame codec ------------------------------------------------------ */

int triple_encode_frame(const struct triple_frame *f, u8 *out, size_t outsz)
{
  u8 tmp[TRIPLE_MTU];
  u8 flags;
  int n = 0;
  int i;

  if (f->port >= TRIPLE_PORTS || f->len > TRIPLE_MAX_DATA || (!f->fd && f->len > 8))
    return -1;

  tmp[n++] = TRIPLE_FIRST_BYTE;
  tmp[n++] = 0;
  n += triple_put(tmp + n, TRIPLE_CMD_TX_CAN_1 + f->port);

  for (i = 0; i < TRIPLE_ID_LEN; i++)
    n += triple_put(tmp + n, (u8)(f->id >> (8 * (TRIPLE_ID_LEN - 1 - i))));

  flags = triple_len2dlc(f->len);
  if (f->eff)
    flags |= TRIPLE_FLAG_EFF;
  if (f->rtr)
    flags |= TRIPLE_FLAG_RTR;
  if (f->fd)
  {
    flags |= TRIPLE_FLAG_FD;
    if (f->brs)
      flags |= TRIPLE_FLAG_BRS;
  }
  n += triple_put(tmp + n, flags);

  if (!f->rtr)
    for (i = 0; i < f->len; i++)
      n += triple_put(tmp + n, f->data[i]);

  tmp[n++] = TRIPLE_LAST_BYTE;
  tmp[1] = (u8)n;

  if ((size_t)n > outsz)
    return -1;
  memcpy(out, tmp, n);
  return n;
}

static u32 get_be32(const u8 *p)
{
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

int triple_decode(const u8 *wire, size_t n, struct triple_msg *m)
{
  u8 buf[TRIPLE_MTU];
  struct triple_frame *f = &m->frame;
  const u8 *p;
  size_t len;
  size_t avail;
  size_t need;
  u8 cmd;
  u8 flags;
  int idx;

  if (n < 4 || n > TRIPLE_MTU || wire[0] != TRIPLE_FIRST_BYTE || wire[n - 1] != TRIPLE_LAST_BYTE)
    return -1;

  len = triple_unescape(buf, wire, n);
  memset(m, 0, sizeof(*m));

  cmd = buf[2];
  p = buf + 3;
  avail = len - 4; /* payload bytes between the command and the end byte */

  switch (cmd)
  {
  case TRIPLE_CMD_STATUS:
    if (avail < TRIPLE_STATUS_LEN)
      return -3;
    m->type = TRIPLE_MSG_STATUS;
    memcpy(m->status.raw, p, TRIPLE_STATUS_LEN);
    /* mapping verified by forcing errors per port, see doc/PLAN.md */
    m->status.port[0].tec = p[1];
    m->status.port[0].rec = p[2];
    m->status.port[1].tec = p[3];
    m->status.port[1].rec = p[4];
    m->status.port[2].tec = p[5];
    m->status.port[2].rec = p[6];
    m->status.port[0].state = p[29];
    m->status.port[1].state = p[30];
    m->status.port[2].state = p[31];
    return m->type;

  case TRIPLE_CMD_FW_VER:
    m->type = TRIPLE_MSG_FW_VER;
    if (avail > sizeof(m->fw_version) - 1)
      avail = sizeof(m->fw_version) - 1;
    memcpy(m->fw_version, p, avail);
    m->fw_version[avail] = 0;
    return m->type;

  case TRIPLE_CMD_RX_CAN_1:
  case TRIPLE_CMD_RX_CAN_2:
  case TRIPLE_CMD_RX_CAN_3:
  case TRIPLE_CMD_RX_CAN_3_ESI:
    idx = cmd - TRIPLE_CMD_RX_CAN_1;
    f->has_ts = false;
    break;

  case TRIPLE_CMD_RX_TS_CAN_1:
  case TRIPLE_CMD_RX_TS_CAN_2:
  case TRIPLE_CMD_RX_TS_CAN_3:
  case TRIPLE_CMD_RX_TS_CAN_3_ESI:
    idx = cmd - TRIPLE_CMD_RX_TS_CAN_1;
    f->has_ts = true;
    break;

  default:
    return -2;
  }

  m->type = TRIPLE_MSG_FRAME;
  f->port = idx < 3 ? idx : 2;
  f->esi = idx == 3;

  need = TRIPLE_ID_LEN + 1 + (f->has_ts ? TRIPLE_TS_LEN : 0);
  if (avail < need)
    return -3;

  f->id = get_be32(p);
  flags = p[TRIPLE_ID_LEN];
  p += TRIPLE_ID_LEN + 1;
  if (f->has_ts)
  {
    f->ts = get_be32(p);
    p += TRIPLE_TS_LEN;
  }

  f->eff = !!(flags & TRIPLE_FLAG_EFF);
  f->rtr = !!(flags & TRIPLE_FLAG_RTR);
  f->fd = !!(flags & TRIPLE_FLAG_FD);
  f->brs = f->fd && !!(flags & TRIPLE_FLAG_BRS);
  f->len = triple_dlc2len(flags & TRIPLE_FLAG_DLC);

  if (!f->fd && f->len > 8)
    return -4;

  if (!f->rtr)
  {
    if (avail - need < f->len)
      return -5;
    memcpy(f->data, p, f->len);
  }
  return m->type;
}

/* ---- timestamps ------------------------------------------------------- */

u64 triple_ts_extend(struct triple_ts *t, u32 ts)
{
  const u64 wrap = (u64)1 << 32;
  u32 back = t->last - ts;          /* modulo 2^32 */

  if (t->valid && back != 0 && back <= TRIPLE_TS_REORDER_US)
  {
    /* slightly older than the newest frame: reordered between ports. If
     * the newest frame already crossed a wrap, this one is from before it. */
    if (ts > t->last && t->base >= wrap)
      return t->base - wrap + ts;
    return t->base + ts;
  }
  if (t->valid && ts < t->last)
    t->base += wrap;                /* counter wrapped (or the adapter restarted) */
  t->last = ts;
  t->valid = true;
  return t->base + ts;
}

/* ---- commands --------------------------------------------------------- */

int triple_encode_settings_can(u8 *out, u8 port, u16 kbit, bool listen_only)
{
  u8 payload[4] = { port, (u8)(kbit >> 8), (u8)kbit, listen_only ? 1 : 0 };

  return triple_build(out, TRIPLE_CMD_SETTINGS, payload, sizeof(payload));
}

int triple_encode_settings_fd(u8 *out, u32 code, bool listen_only, bool iso, bool esi)
{
  u8 payload[8] = { 3, (u8)(code >> 24), (u8)(code >> 16), (u8)(code >> 8), (u8)code,
                    listen_only ? 1 : 0, iso ? 1 : 0, esi ? 1 : 0 };

  return triple_build(out, TRIPLE_CMD_SETTINGS, payload, sizeof(payload));
}

int triple_encode_bittiming_fd(u8 *out, const struct triple_fd_bittiming *bt, bool listen_only, bool iso, bool esi)
{
  const u16 v[11] = { bt->nbrp, bt->ntseg1, bt->ntseg2, bt->nsjw, bt->dbrp, bt->dtseg1, bt->dtseg2, bt->dsjw,
                      bt->tdco, bt->tdcv, bt->tdcmod };
  u8 payload[1 + 22 + 3];
  int i;

  payload[0] = 3;
  for (i = 0; i < 11; i++)
  {
    payload[1 + 2 * i] = (u8)(v[i] >> 8);
    payload[2 + 2 * i] = (u8)v[i];
  }
  payload[23] = listen_only ? 1 : 0;
  payload[24] = iso ? 1 : 0;
  payload[25] = esi ? 1 : 0;
  return triple_build(out, TRIPLE_CMD_BITTIMING, payload, sizeof(payload));
}

int triple_encode_timestamp_mode(u8 *out, bool on)
{
  u8 payload[1] = { on ? 1 : 0 };

  return triple_build(out, TRIPLE_CMD_TIMESTAMP, payload, sizeof(payload));
}

int triple_encode_buffer_mode(u8 *out, u32 mode)
{
  u8 payload[4] = { (u8)(mode >> 24), (u8)(mode >> 16), (u8)(mode >> 8), (u8)mode };

  return triple_build(out, TRIPLE_CMD_BUFFER_MODE, payload, sizeof(payload));
}

int triple_encode_fw_version_req(u8 *out)
{
  return triple_build(out, TRIPLE_CMD_FW_VER, NULL, 0);
}

/* ---- bitrate tables --------------------------------------------------- */

static const struct { u32 bitrate; u16 kbit; } can_speeds[] = {
  { 10000, 10 }, { 20000, 20 }, { 33333, 33 }, { 50000, 50 }, { 62500, 62 },
  { 83333, 83 }, { 100000, 100 }, { 125000, 125 }, { 250000, 250 },
  { 500000, 500 }, { 1000000, 1000 },
};

u16 triple_can_speed_kbit(u32 bitrate)
{
  size_t i;

  for (i = 0; i < sizeof(can_speeds) / sizeof(can_speeds[0]); i++)
    if (can_speeds[i].bitrate == bitrate)
      return can_speeds[i].kbit;
  return 0;
}

static const u32 fd_nominal[] = { 125000, 250000, 500000, 1000000 };

static const struct { u32 bitrate; u16 code; } fd_data[] = {
  { 250000, 250 }, { 500000, 500 }, { 833333, 833 }, { 1000000, 1000 },
  { 1500000, 1500 }, { 2000000, 2000 }, { 3000000, 3000 }, { 4000000, 4000 },
  { 5000000, 5000 }, { 6700000, 6700 }, { 8000000, 8000 }, { 10000000, 9999 },
};

/* Firmware codes concatenate the two kbit values: 125k/250k -> 125250,
 * 125k/1M -> 1251000, 1M/1.5M -> 10001500, 10M is written as 9999. */
u32 triple_fd_speed_code(u32 nominal, u32 data)
{
  size_t i;
  u32 nom_k = 0;
  u16 data_code = 0;

  for (i = 0; i < sizeof(fd_nominal) / sizeof(fd_nominal[0]); i++)
    if (fd_nominal[i] == nominal)
      nom_k = nominal / 1000;
  for (i = 0; i < sizeof(fd_data) / sizeof(fd_data[0]); i++)
    if (fd_data[i].bitrate == data)
      data_code = fd_data[i].code;

  if (!nom_k || !data_code || data <= nominal)
    return 0;

  if (data_code >= 1000)
    return nom_k * 10000 + data_code;
  return nom_k * 1000 + data_code;
}

u32 triple_fd_default_data_bitrate(u32 nominal)
{
  size_t i;

  for (i = 0; i < sizeof(fd_data) / sizeof(fd_data[0]); i++)
    if (fd_data[i].bitrate > nominal)
      return fd_data[i].bitrate;
  return 0;
}
