// SPDX-License-Identifier: GPL-2.0
/*
 * CANLAB USB2CAN Triple SocketCAN driver (tty line discipline).
 *
 * The adapter enumerates as a CDC ACM serial port. This line discipline
 * turns one such port into three CAN network devices (two CAN 2.0, one
 * CAN FD) using the CAN device API, so that bitrates and modes are set
 * with `ip link set canX type can ...` and the driver itself talks to the
 * firmware. Attach it with `ldattach 29 /dev/ttyACMx`.
 *
 * Structure follows drivers/net/can/slcan (Oliver Hartkopp, Dario
 * Binacchi) and the earlier Canlab / Innodisk EMUC derived driver.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2.
 * The protocol codec it links (triple_proto.c) is MIT licensed, which is
 * compatible; the module as a whole is GPL-2.0.
 */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/sockios.h>
#include <linux/uaccess.h>
#include <linux/ethtool.h>
#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/skb.h>
#include <linux/can/error.h>

#include "triple_proto.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 14, 0)
#error "this driver needs Linux 5.14 or newer"
#endif

/* N_DEVELOPMENT: reserved for out-of-tree line disciplines */
#define N_TRIPLE 29

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CANLAB USB2CAN Triple SocketCAN line discipline");
MODULE_AUTHOR("Canlab");
MODULE_VERSION("v2.0");
MODULE_ALIAS_LDISC(N_TRIPLE);

static int max_wire_len = TRIPLE_FW_MAX_WIRE_LEN;
module_param(max_wire_len, int, 0644);
MODULE_PARM_DESC(max_wire_len, "drop frames longer than this on the wire, escapes included (default 100)");

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "hex dump every message exchanged with the adapter");

static bool hwtstamp = true;
module_param(hwtstamp, bool, 0444);
MODULE_PARM_DESC(hwtstamp, "ask the adapter for 1 us receive timestamps (default on; off raises the sustainable receive rate by about a third)");

static bool fd_bittiming;
module_param(fd_bittiming, bool, 0444);
MODULE_PARM_DESC(fd_bittiming, "CAN3: any bitrate and sample point, bit timing computed by the kernel and sent as raw "
                                "controller timing, instead of the fixed bitrate table (default off)");

/* CAN FD parameters moved into struct can_priv::fd in 6.16 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#define TRIPLE_FD(priv) ((priv)->fd)
#else
#define TRIPLE_FD(priv) (*(priv))
#endif

struct triple_adapter;

struct triple_port {
  struct can_priv can;              /* must be first */
  struct triple_adapter *adapter;
  struct net_device *dev;
  u8 index;                         /* 0..2 */
  struct can_berr_counter bec;      /* from the last status message */
};

struct triple_adapter {
  struct tty_struct *tty;
  struct net_device *devs[TRIPLE_PORTS];

  spinlock_t lock;
  struct work_struct tx_work;
  wait_queue_head_t xcmd_wait;
  unsigned long flags;
#define TF_ERROR 0                  /* rx stream out of sync, resyncing */
#define TF_ESC   1                  /* next rx byte is escaped */
#define TF_XCMD  2                  /* a command (not a frame) is in flight */

  u8 rbuff[TRIPLE_MTU];
  int rcount;

  u8 xbuff[TRIPLE_MTU];
  u8 *xhead;
  int xleft;
  int tx_port;                      /* port owning the frame in flight, -1 none */

  struct triple_ts ts;              /* 32 bit microsecond counter extension */

  char fw_version[16];
};

static const u32 triple_can_bitrates[] = {
  10000, 20000, 33333, 50000, 62500, 83333, 100000, 125000, 250000, 500000, 1000000,
};

static const u32 triple_fd_nominal_bitrates[] = { 125000, 250000, 500000, 1000000 };

static const u32 triple_fd_data_bitrates[] = {
  250000, 500000, 833333, 1000000, 1500000, 2000000, 3000000, 4000000, 5000000,
  6700000, 8000000, 10000000,
};

/* CAN3 controller (MCP2517FD at 40 MHz) limits, for fd_bittiming=1 */
static const struct can_bittiming_const triple_fd_nominal_const = {
  .name = "mcp2517fd",
  .tseg1_min = 2, .tseg1_max = 256,
  .tseg2_min = 1, .tseg2_max = 128,
  .sjw_max = 128,
  .brp_min = 1, .brp_max = 256, .brp_inc = 1,
};

static const struct can_bittiming_const triple_fd_data_const = {
  .name = "mcp2517fd",
  .tseg1_min = 1, .tseg1_max = 32,
  .tseg2_min = 1, .tseg2_max = 16,
  .sjw_max = 16,
  .brp_min = 1, .brp_max = 256, .brp_inc = 1,
};

/* ------------------------------------------------------------------------
 * transmit side
 */

/* Called with adapter->lock held. Finishes the frame in flight. */
static void triple_tx_complete(struct triple_adapter *a)
{
  struct net_device *dev;

  if (a->tx_port < 0)
    return;
  dev = a->devs[a->tx_port];
  a->tx_port = -1;
  dev->stats.tx_packets++;
  dev->stats.tx_bytes += can_get_echo_skb(dev, 0, NULL);
}

static void triple_wake_queues(struct triple_adapter *a)
{
  int i;

  for (i = 0; i < TRIPLE_PORTS; i++)
    if (netif_running(a->devs[i]))
      netif_wake_queue(a->devs[i]);
}

/* Work item: push remaining bytes to the tty, complete when drained. */
static void triple_transmit(struct work_struct *work)
{
  struct triple_adapter *a = container_of(work, struct triple_adapter, tx_work);
  int actual;

  spin_lock_bh(&a->lock);
  if (!a->tty)
  {
    spin_unlock_bh(&a->lock);
    return;
  }

  if (a->xleft <= 0)
  {
    clear_bit(TTY_DO_WRITE_WAKEUP, &a->tty->flags);
    if (test_and_clear_bit(TF_XCMD, &a->flags))
    {
      spin_unlock_bh(&a->lock);
      wake_up(&a->xcmd_wait);
    }
    else
    {
      triple_tx_complete(a);
      spin_unlock_bh(&a->lock);
    }
    triple_wake_queues(a);
    return;
  }

  actual = a->tty->ops->write(a->tty, a->xhead, a->xleft);
  if (actual < 0)
    actual = 0;
  a->xleft -= actual;
  a->xhead += actual;
  spin_unlock_bh(&a->lock);
}

static void triple_write_wakeup(struct tty_struct *tty)
{
  struct triple_adapter *a = tty->disc_data;

  if (a)
    schedule_work(&a->tx_work);
}

/* Send a command and wait until the tty has taken all of it. */
static int triple_send_cmd(struct triple_adapter *a, const u8 *buf, int len)
{
  unsigned long deadline = jiffies + HZ;
  int actual;
  int ret;

  if (debug)
    print_hex_dump(KERN_DEBUG, "triple cmd: ", DUMP_PREFIX_NONE, 32, 1, buf, len, false);

  for (;;)
  {
    spin_lock_bh(&a->lock);
    if (!a->tty)
    {
      spin_unlock_bh(&a->lock);
      return -ENODEV;
    }
    if (a->xleft <= 0 && !test_bit(TF_XCMD, &a->flags))
      break;
    spin_unlock_bh(&a->lock);
    if (time_after(jiffies, deadline))
      return -EBUSY;
    usleep_range(500, 1000);
  }

  triple_tx_complete(a);
  memcpy(a->xbuff, buf, len);
  set_bit(TF_XCMD, &a->flags);
  set_bit(TTY_DO_WRITE_WAKEUP, &a->tty->flags);
  actual = a->tty->ops->write(a->tty, a->xbuff, len);
  if (actual < 0)
    actual = 0;
  a->xleft = len - actual;
  a->xhead = a->xbuff + actual;
  spin_unlock_bh(&a->lock);

  if (a->xleft == 0)
    schedule_work(&a->tx_work);

  ret = wait_event_interruptible_timeout(a->xcmd_wait, !test_bit(TF_XCMD, &a->flags), HZ);
  clear_bit(TF_XCMD, &a->flags);
  if (ret == -ERESTARTSYS)
    return ret;
  if (ret == 0)
    return -ETIMEDOUT;
  return 0;
}

static bool triple_dropped_skb(struct net_device *dev, struct sk_buff *skb)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
  return can_dev_dropped_skb(dev, skb);
#else
  struct can_priv *priv = netdev_priv(dev);

  if (priv->ctrlmode & CAN_CTRLMODE_LISTENONLY)
  {
    kfree_skb(skb);
    dev->stats.tx_dropped++;
    return true;
  }
  return can_dropped_invalid_skb(dev, skb);
#endif
}

static netdev_tx_t triple_xmit(struct sk_buff *skb, struct net_device *dev)
{
  struct triple_port *port = netdev_priv(dev);
  struct triple_adapter *a = port->adapter;
  struct triple_frame f;
  canid_t id;
  int len;
  int actual;
  int i;

  if (triple_dropped_skb(dev, skb))
    return NETDEV_TX_OK;

  spin_lock(&a->lock);

  if (!a->tty || !netif_running(dev))
  {
    spin_unlock(&a->lock);
    goto drop;
  }

  /* One frame in flight per adapter: the three ports share the tty. */
  if (a->xleft > 0 || a->tx_port >= 0 || test_bit(TF_XCMD, &a->flags))
  {
    netif_stop_queue(dev);
    spin_unlock(&a->lock);
    return NETDEV_TX_BUSY;
  }

  memset(&f, 0, sizeof(f));
  f.port = port->index;

  if (skb->len == CANFD_MTU)
  {
    struct canfd_frame *cfd = (struct canfd_frame *)skb->data;

    id = cfd->can_id;
    f.fd = true;
    f.brs = !!(cfd->flags & CANFD_BRS);
    f.esi = !!(cfd->flags & CANFD_ESI);
    f.len = min_t(u8, cfd->len, TRIPLE_MAX_DATA);
    memcpy(f.data, cfd->data, f.len);
  }
  else
  {
    struct can_frame *cf = (struct can_frame *)skb->data;

    id = cf->can_id;
    f.len = min_t(u8, cf->len, 8);
    memcpy(f.data, cf->data, f.len);
  }

  f.rtr = !!(id & CAN_RTR_FLAG);
  f.eff = !!(id & CAN_EFF_FLAG);
  f.id = id & (f.eff ? CAN_EFF_MASK : CAN_SFF_MASK);

  len = triple_encode_frame(&f, a->xbuff, sizeof(a->xbuff));
  if (len < 0 || len > max_wire_len)
  {
    spin_unlock(&a->lock);
    if (net_ratelimit())
      netdev_warn(dev, "frame too long for the adapter (%d > %d bytes on the wire), dropped\n", len, max_wire_len);
    goto drop;
  }

  if (debug)
    print_hex_dump(KERN_DEBUG, "triple tx:  ", DUMP_PREFIX_NONE, 32, 1, a->xbuff, len, false);

  for (i = 0; i < TRIPLE_PORTS; i++)
    netif_stop_queue(a->devs[i]);

  can_put_echo_skb(skb, dev, 0, 0);
  a->tx_port = port->index;

  /* Request the wakeup before writing: a short write may complete inside
   * ops->write() and the wakeup would otherwise never come. */
  set_bit(TTY_DO_WRITE_WAKEUP, &a->tty->flags);
  actual = a->tty->ops->write(a->tty, a->xbuff, len);
  if (actual < 0)
    actual = 0;
  a->xleft = len - actual;
  a->xhead = a->xbuff + actual;
  spin_unlock(&a->lock);

  if (a->xleft == 0)
    schedule_work(&a->tx_work);

  return NETDEV_TX_OK;

drop:
  dev->stats.tx_dropped++;
  kfree_skb(skb);
  return NETDEV_TX_OK;
}

/* ------------------------------------------------------------------------
 * receive side
 */

static void triple_rx_frame(struct triple_adapter *a, const struct triple_frame *f)
{
  struct net_device *dev = a->devs[f->port];
  struct sk_buff *skb;
  canid_t id = f->id;

  if (!netif_running(dev))
    return;

  if (f->eff)
    id |= CAN_EFF_FLAG;
  if (f->rtr)
    id |= CAN_RTR_FLAG;

  if (f->fd)
  {
    struct canfd_frame *cfd;

    skb = alloc_canfd_skb(dev, &cfd);
    if (!skb)
      goto dropped;
    cfd->can_id = id;
    cfd->len = f->rtr ? 0 : f->len;
    if (f->brs)
      cfd->flags |= CANFD_BRS;
    if (f->esi)
      cfd->flags |= CANFD_ESI;
    memcpy(cfd->data, f->data, cfd->len);
    dev->stats.rx_bytes += cfd->len;
  }
  else
  {
    struct can_frame *cf;

    skb = alloc_can_skb(dev, &cf);
    if (!skb)
      goto dropped;
    cf->can_id = id;
    cf->len = f->rtr ? 0 : f->len;
    memcpy(cf->data, f->data, cf->len);
    dev->stats.rx_bytes += cf->len;
  }

  if (f->has_ts)
    skb_hwtstamps(skb)->hwtstamp = ns_to_ktime(triple_ts_extend(&a->ts, f->ts) * NSEC_PER_USEC);

  dev->stats.rx_packets++;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
  netif_rx(skb);
#else
  netif_rx_ni(skb);
#endif
  return;

dropped:
  dev->stats.rx_dropped++;
}

static enum can_state triple_counter_state(u8 cnt)
{
  if (cnt >= 128)
    return CAN_STATE_ERROR_PASSIVE;
  if (cnt >= 96)
    return CAN_STATE_ERROR_WARNING;
  return CAN_STATE_ERROR_ACTIVE;
}

static void triple_rx_status(struct triple_adapter *a, const struct triple_status *st)
{
  int i;

  for (i = 0; i < TRIPLE_PORTS; i++)
  {
    struct net_device *dev = a->devs[i];
    struct triple_port *port = netdev_priv(dev);
    const struct triple_port_status *ps = &st->port[i];
    enum can_state tx_state, rx_state, new_state;
    struct can_frame *cf;
    struct sk_buff *skb;

    port->bec.txerr = ps->tec;
    port->bec.rxerr = ps->rec;

    if (!netif_running(dev) || port->can.state == CAN_STATE_BUS_OFF)
      continue;

    tx_state = triple_counter_state(ps->tec);
    rx_state = triple_counter_state(ps->rec);
    /* The firmware state byte: 0 active, 1 passive observed. Anything
     * above is taken as bus off (not yet observed on hardware). */
    if (ps->state >= 2)
      tx_state = rx_state = CAN_STATE_BUS_OFF;

    new_state = max(tx_state, rx_state);
    if (new_state == port->can.state)
      continue;

    skb = alloc_can_err_skb(dev, &cf);
    can_change_state(dev, cf, tx_state, rx_state);
    if (skb)
    {
#ifdef CAN_ERR_CNT
      cf->can_id |= CAN_ERR_CNT;
#endif
      cf->data[6] = ps->tec;
      cf->data[7] = ps->rec;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
      netif_rx(skb);
#else
      netif_rx_ni(skb);
#endif
    }
    if (new_state == CAN_STATE_BUS_OFF)
      can_bus_off(dev);
  }
}

static void triple_rx_msg(struct triple_adapter *a)
{
  struct triple_msg m;
  int type;

  if (debug)
    print_hex_dump(KERN_DEBUG, "triple rx:  ", DUMP_PREFIX_NONE, 32, 1, a->rbuff, a->rcount, false);

  type = triple_decode(a->rbuff, a->rcount, &m);
  if (type < 0)
  {
    if (debug)
      pr_debug("triple: dropped message, decode error %d (%d bytes)\n", type, a->rcount);
    return;
  }

  switch (type)
  {
  case TRIPLE_MSG_FRAME:
    triple_rx_frame(a, &m.frame);
    break;
  case TRIPLE_MSG_STATUS:
    triple_rx_status(a, &m.status);
    break;
  case TRIPLE_MSG_FW_VER:
    if (strcmp(a->fw_version, m.fw_version))
    {
      strscpy(a->fw_version, m.fw_version, sizeof(a->fw_version));
      pr_info("triple: %s firmware %s\n", a->tty->name, a->fw_version);
    }
    break;
  }
}

/* One byte from the tty. Bytes are kept raw (escaped) until the end byte. */
static void triple_rx_byte(struct triple_adapter *a, u8 s)
{
  bool escaped;

  if (test_bit(TF_ERROR, &a->flags))
  {
    if (s == TRIPLE_LAST_BYTE)
    {
      clear_bit(TF_ERROR, &a->flags);
      clear_bit(TF_ESC, &a->flags);
      a->rcount = 0;
    }
    return;
  }

  if (a->rcount >= TRIPLE_MTU)
  {
    int i;

    for (i = 0; i < TRIPLE_PORTS; i++)
      a->devs[i]->stats.rx_over_errors++;
    set_bit(TF_ERROR, &a->flags);
    clear_bit(TF_ESC, &a->flags);
    a->rcount = 0;
    return;
  }

  if (a->rcount == 0 && s != TRIPLE_FIRST_BYTE)
    return;

  /* byte 1 is the raw length: never an escape marker or end byte */
  if (a->rcount == 1)
  {
    a->rbuff[a->rcount++] = s;
    return;
  }

  escaped = test_and_clear_bit(TF_ESC, &a->flags);
  a->rbuff[a->rcount++] = s;
  if (escaped)
    return;

  if (s == TRIPLE_ESC_BYTE)
  {
    set_bit(TF_ESC, &a->flags);
    return;
  }

  if (s == TRIPLE_LAST_BYTE)
  {
    triple_rx_msg(a);
    a->rcount = 0;
  }
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
static void triple_receive_buf(struct tty_struct *tty, const u8 *cp, const u8 *fp, size_t count)
#else
static void triple_receive_buf(struct tty_struct *tty, const unsigned char *cp, const char *fp, int count)
#endif
{
  struct triple_adapter *a = tty->disc_data;

  if (!a)
    return;

  while (count--)
  {
    if (fp && *fp++)
    {
      /* tty framing error: resync on the next end byte */
      if (!test_and_set_bit(TF_ERROR, &a->flags))
      {
        int i;

        for (i = 0; i < TRIPLE_PORTS; i++)
          a->devs[i]->stats.rx_errors++;
      }
      cp++;
      continue;
    }
    triple_rx_byte(a, *cp++);
  }
}

/* ------------------------------------------------------------------------
 * netdevice side
 */

/* CAN3 with fd_bittiming=1: the bit timing the CAN core computed (or the
 * user gave in time quanta) as raw controller timing, command 0x89. */
static int triple_fd_raw_timing_cmd(struct triple_port *port, u8 *cmd, bool listen, int *n)
{
  const struct can_bittiming *nb = &port->can.bittiming;
  const struct can_bittiming *db = &TRIPLE_FD(&port->can).data_bittiming;
  bool fd = !!(port->can.ctrlmode & CAN_CTRLMODE_FD);
  bool iso = !(port->can.ctrlmode & CAN_CTRLMODE_FD_NON_ISO);
  struct triple_fd_bittiming bt;
  u32 dbrp, dtseg1, dtseg2, dsjw, data;

  if (fd)
  {
    dbrp = db->brp;
    dtseg1 = db->prop_seg + db->phase_seg1;
    dtseg2 = db->phase_seg2;
    dsjw = db->sjw;
    data = db->bitrate;
  }
  else
  {
    /* the controller always runs CAN FD; give the data phase a rate above
     * the nominal one so frames with BRS from other nodes still decode */
    data = triple_fd_default_data_bitrate(nb->bitrate);
    data = triple_fd_data_timing(data ? data : 2000000, &dbrp, &dtseg1, &dtseg2, &dsjw);
  }
  if (!data || triple_fd_bittiming_from_tq(&bt, nb->brp, nb->prop_seg + nb->phase_seg1, nb->phase_seg2, nb->sjw,
                                           dbrp, dtseg1, dtseg2, dsjw) < 0)
  {
    netdev_err(port->dev, "bit timing outside the controller's range\n");
    return -EINVAL;
  }
  *n = triple_encode_bittiming_fd(cmd, &bt, listen, iso, false);
  netdev_info(port->dev, "%s %u / %u bit/s (raw timing: brp %u tseg1 %u tseg2 %u sjw %u / brp %u tseg1 %u tseg2 %u sjw %u, tdc %s)%s%s\n",
              fd ? "CAN FD" : "CAN (FD data phase kept for other nodes)",
              nb->bitrate, data, bt.nbrp + 1, bt.ntseg1 + 1, bt.ntseg2 + 1, bt.nsjw + 1,
              bt.dbrp + 1, bt.dtseg1 + 1, bt.dtseg2 + 1, bt.dsjw + 1, bt.tdcmod ? "auto" : "off",
              iso ? "" : ", non-ISO", listen ? ", listen only" : "");
  return 0;
}

static int triple_netdev_open(struct net_device *dev)
{
  struct triple_port *port = netdev_priv(dev);
  struct triple_adapter *a = port->adapter;
  bool listen = !!(port->can.ctrlmode & CAN_CTRLMODE_LISTENONLY);
  u8 cmd[TRIPLE_MTU];
  int n;
  int err;

  if (!a->tty)
    return -ENODEV;

  err = open_candev(dev);
  if (err)
    return err;

  if (port->index < 2)
  {
    u16 kbit = triple_can_speed_kbit(port->can.bittiming.bitrate);

    if (!kbit)
    {
      err = -EINVAL;
      goto fail;
    }
    n = triple_encode_settings_can(cmd, port->index + 1, kbit, listen);
    netdev_info(dev, "CAN %u kbit/s%s\n", kbit, listen ? ", listen only" : "");
  }
  else if (fd_bittiming)
  {
    err = triple_fd_raw_timing_cmd(port, cmd, listen, &n);
    if (err)
      goto fail;
  }
  else
  {
    u32 nominal = port->can.bittiming.bitrate;
    u32 data = (port->can.ctrlmode & CAN_CTRLMODE_FD) ? TRIPLE_FD(&port->can).data_bittiming.bitrate : 0;
    bool iso = !(port->can.ctrlmode & CAN_CTRLMODE_FD_NON_ISO);
    u32 code;

    if (!data)
      data = triple_fd_default_data_bitrate(nominal);
    code = triple_fd_speed_code(nominal, data);
    if (!code)
    {
      netdev_err(dev, "unsupported bitrate pair %u / %u (nominal 125k, 250k, 500k, 1M; data must be higher)\n", nominal, data);
      err = -EINVAL;
      goto fail;
    }
    n = triple_encode_settings_fd(cmd, code, listen, iso, false);
    netdev_info(dev, "CAN FD %u / %u bit/s%s%s\n", nominal, data, iso ? "" : ", non-ISO", listen ? ", listen only" : "");
  }

  err = triple_send_cmd(a, cmd, n);
  if (err)
  {
    netdev_err(dev, "failed to send the bitrate command to the adapter (%d)\n", err);
    goto fail;
  }

  port->can.state = CAN_STATE_ERROR_ACTIVE;
  netif_start_queue(dev);
  return 0;

fail:
  close_candev(dev);
  return err;
}

static int triple_netdev_close(struct net_device *dev)
{
  struct triple_port *port = netdev_priv(dev);
  struct triple_adapter *a = port->adapter;

  netif_stop_queue(dev);

  spin_lock_bh(&a->lock);
  if (a->tx_port == port->index)
    a->tx_port = -1; /* close_candev() flushes the echo skb */
  spin_unlock_bh(&a->lock);

  close_candev(dev);
  port->can.state = CAN_STATE_STOPPED;
  return 0;
}

static int triple_set_mode(struct net_device *dev, enum can_mode mode)
{
  struct triple_port *port = netdev_priv(dev);

  switch (mode)
  {
  case CAN_MODE_START:
    /* The firmware recovers from bus off on its own; just resume. */
    port->can.state = CAN_STATE_ERROR_ACTIVE;
    netif_wake_queue(dev);
    return 0;
  default:
    return -EOPNOTSUPP;
  }
}

static int triple_get_berr_counter(const struct net_device *dev, struct can_berr_counter *bec)
{
  const struct triple_port *port = netdev_priv(dev);

  *bec = port->bec;
  return 0;
}

static const struct net_device_ops triple_netdev_ops = {
  .ndo_open = triple_netdev_open,
  .ndo_stop = triple_netdev_close,
  .ndo_start_xmit = triple_xmit,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
  /* MTU is fixed by the CAN core; timestamping moved to dedicated hooks */
  .ndo_hwtstamp_get = can_hwtstamp_get,
  .ndo_hwtstamp_set = can_hwtstamp_set,
#else
  .ndo_change_mtu = can_change_mtu,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
  .ndo_eth_ioctl = can_eth_ioctl_hwts,
#endif
#endif
};

static const struct ethtool_ops triple_ethtool_ops = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
  .get_ts_info = can_ethtool_op_get_ts_info_hwts,
#else
  .get_ts_info = ethtool_op_get_ts_info,
#endif
};

/* ------------------------------------------------------------------------
 * line discipline
 */

static void triple_free_adapter(struct triple_adapter *a)
{
  int i;

  for (i = 0; i < TRIPLE_PORTS; i++)
    if (a->devs[i])
      free_candev(a->devs[i]);
  kfree(a);
}

static int triple_ldisc_open(struct tty_struct *tty)
{
  struct triple_adapter *a;
  u8 cmd[16];
  int i;
  int err;

  if (!capable(CAP_NET_ADMIN))
    return -EPERM;
  if (!tty->ops->write)
    return -EOPNOTSUPP;

  a = kzalloc(sizeof(*a), GFP_KERNEL);
  if (!a)
    return -ENOMEM;

  spin_lock_init(&a->lock);
  INIT_WORK(&a->tx_work, triple_transmit);
  init_waitqueue_head(&a->xcmd_wait);
  a->tx_port = -1;

  for (i = 0; i < TRIPLE_PORTS; i++)
  {
    struct net_device *dev;
    struct triple_port *port;

    dev = alloc_candev(sizeof(struct triple_port), 1);
    if (!dev)
    {
      err = -ENOMEM;
      goto free;
    }
    a->devs[i] = dev;

    port = netdev_priv(dev);
    port->adapter = a;
    port->dev = dev;
    port->index = i;
    port->can.ctrlmode_supported = CAN_CTRLMODE_LISTENONLY;
    port->can.do_set_mode = triple_set_mode;
    port->can.do_get_berr_counter = triple_get_berr_counter;

    if (i < 2)
    {
      port->can.bitrate_const = triple_can_bitrates;
      port->can.bitrate_const_cnt = ARRAY_SIZE(triple_can_bitrates);
    }
    else if (fd_bittiming)
    {
      /* the CAN core takes either fixed bitrates or timing limits, not both */
      port->can.clock.freq = TRIPLE_FD_CLOCK_HZ;
      port->can.bittiming_const = &triple_fd_nominal_const;
      TRIPLE_FD(&port->can).data_bittiming_const = &triple_fd_data_const;
      port->can.ctrlmode_supported |= CAN_CTRLMODE_FD | CAN_CTRLMODE_FD_NON_ISO;
    }
    else
    {
      port->can.bitrate_const = triple_fd_nominal_bitrates;
      port->can.bitrate_const_cnt = ARRAY_SIZE(triple_fd_nominal_bitrates);
      TRIPLE_FD(&port->can).data_bitrate_const = triple_fd_data_bitrates;
      TRIPLE_FD(&port->can).data_bitrate_const_cnt = ARRAY_SIZE(triple_fd_data_bitrates);
      port->can.ctrlmode_supported |= CAN_CTRLMODE_FD | CAN_CTRLMODE_FD_NON_ISO;
    }

    dev->netdev_ops = &triple_netdev_ops;
    dev->ethtool_ops = &triple_ethtool_ops;
    /* We loop transmitted frames back ourselves (echo skb) once the
     * adapter has taken them, instead of af_can doing it on send. */
    dev->flags |= IFF_ECHO;
    /* sysfs dev_port = CAN connector number, for udev naming rules */
    dev->dev_port = i;
    if (tty->dev)
      SET_NETDEV_DEV(dev, tty->dev);
  }

  tty->receive_room = 65536;
  tty->disc_data = a;
  a->tty = tty;

  for (i = 0; i < TRIPLE_PORTS; i++)
  {
    err = register_candev(a->devs[i]);
    if (err)
    {
      pr_err("triple: can't register CAN device %d (%d)\n", i, err);
      while (--i >= 0)
        unregister_candev(a->devs[i]);
      goto detach;
    }
  }

  pr_info("triple: %s -> %s %s %s\n", tty->name, a->devs[0]->name, a->devs[1]->name, a->devs[2]->name);

  /* Adapter wide settings: buffered mode, hardware timestamps, firmware
   * version (the reply is logged when it arrives). */
  triple_send_cmd(a, cmd, triple_encode_buffer_mode(cmd, 1));
  msleep(20);
  triple_send_cmd(a, cmd, triple_encode_timestamp_mode(cmd, hwtstamp));
  msleep(20);
  triple_send_cmd(a, cmd, triple_encode_fw_version_req(cmd));

  return 0;

detach:
  tty->disc_data = NULL;
  a->tty = NULL;
free:
  triple_free_adapter(a);
  return err;
}

static void triple_ldisc_close(struct tty_struct *tty)
{
  struct triple_adapter *a = tty->disc_data;
  int i;

  /* hangup followed by close: the second call finds nothing to do */
  if (!a || a->tty != tty)
    return;

  for (i = 0; i < TRIPLE_PORTS; i++)
    unregister_candev(a->devs[i]);

  flush_work(&a->tx_work);

  spin_lock_bh(&a->lock);
  tty->disc_data = NULL;
  a->tty = NULL;
  spin_unlock_bh(&a->lock);

  pr_info("triple: %s detached\n", tty->name);
  triple_free_adapter(a);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
static void triple_ldisc_hangup(struct tty_struct *tty)
{
  triple_ldisc_close(tty);
}
#else
static int triple_ldisc_hangup(struct tty_struct *tty)
{
  triple_ldisc_close(tty);
  return 0;
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
static int triple_ldisc_ioctl(struct tty_struct *tty, unsigned int cmd, unsigned long arg)
#else
static int triple_ldisc_ioctl(struct tty_struct *tty, struct file *file, unsigned int cmd, unsigned long arg)
#endif
{
  struct triple_adapter *a = tty->disc_data;

  if (!a)
    return -EINVAL;

  switch (cmd)
  {
  case SIOCGIFNAME:
    /* name of the first port; the others follow it numerically */
    if (copy_to_user((void __user *)arg, a->devs[0]->name, strlen(a->devs[0]->name) + 1))
      return -EFAULT;
    return 0;
  case SIOCSIFHWADDR:
    return -EINVAL;
  default:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
    return tty_mode_ioctl(tty, cmd, arg);
#else
    return tty_mode_ioctl(tty, file, cmd, arg);
#endif
  }
}

static struct tty_ldisc_ops triple_ldisc = {
  .owner = THIS_MODULE,
  .num = N_TRIPLE,
  .name = "triplecan",
  .open = triple_ldisc_open,
  .close = triple_ldisc_close,
  .hangup = triple_ldisc_hangup,
  .ioctl = triple_ldisc_ioctl,
  .receive_buf = triple_receive_buf,
  .write_wakeup = triple_write_wakeup,
};

static int __init triple_init(void)
{
  int status;

  status = tty_register_ldisc(&triple_ldisc);
  if (status)
    pr_err("triple: can't register line discipline %d (%d)\n", N_TRIPLE, status);
  else
    pr_info("triple: USB2CAN Triple SocketCAN driver, line discipline %d\n", N_TRIPLE);
  return status;
}

static void __exit triple_exit(void)
{
  tty_unregister_ldisc(&triple_ldisc);
}

module_init(triple_init);
module_exit(triple_exit);
