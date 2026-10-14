#include <linux/skbuff.h>
#include <linux/version.h>
#include <linux/tty.h>
#include <linux/mutex.h>
#include <linux/can/skb.h>
#include <linux/can/dev.h>

#include "tx.h"

extern bool trace_func_tran;
extern bool show_debug_tran;
extern int max_wire_len;
extern void print_func_trace(bool is_trace, int line, const char *func);

static void triple_bump(USB2CAN_TRIPLE *adapter);

/*-----------------------------------------------------------------------*/
/* tty byte stream -> framed receive buffer.
 * Bytes are stored raw (still escaped); triple_bump() unescapes them.
 */
void triple_unesc(USB2CAN_TRIPLE *adapter, unsigned char s)
{
  bool escaped;
  int i;

  if (test_bit(SLF_ERROR, &adapter->flags))
  {
    /* Resynchronise on the next frame terminator. */
    if (s == U2C_TR_LAST_BYTE)
    {
      clear_bit(SLF_ERROR, &adapter->flags);
      clear_bit(SLF_ESC, &adapter->flags);
      adapter->rcount = 0;
    }
    return;
  }

  if (adapter->rcount >= TRIPLE_MTU)
  {
    for (i = 0; i < 3; i++)
      adapter->devs[i]->stats.rx_over_errors++;
    set_bit(SLF_ERROR, &adapter->flags);
    clear_bit(SLF_ESC, &adapter->flags);
    adapter->rcount = 0;
    return;
  }

  /* Wait for a frame start. */
  if (adapter->rcount == 0 && s != U2C_TR_FIRST_BYTE)
    return;

  /* Byte 1 is the raw length byte: never an escape marker or terminator. */
  if (adapter->rcount == 1)
  {
    adapter->rbuff[adapter->rcount++] = s;
    return;
  }

  escaped = test_and_clear_bit(SLF_ESC, &adapter->flags);
  adapter->rbuff[adapter->rcount++] = s;

  if (escaped)
    return;

  if (s == U2C_TR_SPEC_BYTE)
  {
    set_bit(SLF_ESC, &adapter->flags);
    return;
  }

  if (s == U2C_TR_LAST_BYTE)
  {
    triple_bump(adapter);
    adapter->rcount = 0;
  }

} /* END: triple_unesc() */

/*-----------------------------------------------------------------------*/
/* Complete frame from the adapter -> decoder -> SocketCAN skb */
static void triple_bump(USB2CAN_TRIPLE *adapter)
{
  TRIPLE_CAN_FRAME frame;
  struct net_device *dev;
  struct sk_buff *skb;
  canid_t id = 0;
  size_t len;
  int ret;
  int i;

  memset(&frame, 0, sizeof(frame));

  if (show_debug_tran)
    print_hex_dump(KERN_DEBUG, "triple raw rx: ", DUMP_PREFIX_NONE, 32, 1,
                   adapter->rbuff, adapter->rcount, false);

  len = triple_unescape_frame(frame.comm_buf, adapter->rbuff, adapter->rcount);

  ret = TripleRecvHex(&frame, len);
  if (ret < 0)
  {
    if (show_debug_tran)
      printk(KERN_DEBUG "triple: rx parse fail %d (len %zu)\n", ret, len);
    return;
  }

  if (ret > 0) /* status or firmware version reply */
  {
    if (show_debug_tran && ret == 2)
      printk(KERN_DEBUG "triple: U2C_TR_CMD_FW_VER\n");
    return;
  }

  if (show_debug_tran)
  {
    printk(KERN_DEBUG "triple: rx port %d fd %d id %02X%02X%02X%02X dlc %d\n",
           frame.CAN_port, frame.fd, frame.id[0], frame.id[1], frame.id[2], frame.id[3], frame.dlc);
  }

  dev = adapter->devs[frame.CAN_port];
  if (!netif_running(dev))
    return;

  for (i = 0; i < ID_LEN; i++)
    id |= (canid_t)frame.id[i] << (8 * (ID_LEN - 1 - i));

  /* Flag bit 0x80 marks an extended identifier in both directions
   * (verified against firmware on 2026-10-14; v1.3 had this inverted). */
  if (frame.id_type)
    id = (id & CAN_EFF_MASK) | CAN_EFF_FLAG;
  else
    id &= CAN_SFF_MASK;

  if (frame.rtr)
    id |= CAN_RTR_FLAG;

  if (!frame.fd)
  {
    struct can_frame *cf;

    skb = alloc_can_skb(dev, &cf);
    if (!skb)
    {
      dev->stats.rx_dropped++;
      return;
    }

    cf->can_id = id;
    /* RTR frames may carry a dlc but never any data bytes */
    CAN_FRAME_LEN(cf) = frame.rtr ? 0 : frame.dlc;
    memcpy(cf->data, frame.data, CAN_FRAME_LEN(cf));
    dev->stats.rx_bytes += CAN_FRAME_LEN(cf);
  }
  else
  {
    struct canfd_frame *cfd;

    skb = alloc_canfd_skb(dev, &cfd);
    if (!skb)
    {
      dev->stats.rx_dropped++;
      return;
    }

    cfd->can_id = id;
    cfd->len = frame.rtr ? 0 : frame.dlc;
    if (frame.fd_br_switch)
      cfd->flags |= CANFD_BRS;
    if (frame.fd_esi)
      cfd->flags |= CANFD_ESI;
    memcpy(cfd->data, frame.data, cfd->len);
    dev->stats.rx_bytes += cfd->len;
  }

  dev->stats.rx_packets++;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
  netif_rx(skb);
#else
  netif_rx_ni(skb);
#endif

} /* END: triple_bump() */

/*-----------------------------------------------------------------------*/
/* SocketCAN skb -> adapter frame -> tty write.
 * Caller holds adapter->lock and has verified that the previous
 * transmission has drained (adapter->xleft == 0).
 */
int triple_encaps(USB2CAN_TRIPLE *adapter, int channel, struct sk_buff *skb)
{
  /*=======================================================*/
  print_func_trace(trace_func_tran, __LINE__, __FUNCTION__);
  /*=======================================================*/

  TRIPLE_CAN_FRAME tf;
  const unsigned char *data;
  canid_t id;
  int dlc;
  int len;
  int actual;
  int i;

  memset(&tf, 0, sizeof(tf));
  tf.CAN_port = channel + 1;

  if (skb->len == CANFD_MTU)
  {
    struct canfd_frame *cfd = (struct canfd_frame *)skb->data;

    tf.fd = true;
    id = cfd->can_id;
    dlc = cfd->len;
    data = cfd->data;
    if (cfd->flags & CANFD_BRS)
      tf.fd_br_switch = true;
    if (cfd->flags & CANFD_ESI)
      tf.fd_esi = true;
    if (dlc > DATA_FD_LEN)
      dlc = DATA_FD_LEN;
  }
  else
  {
    struct can_frame *cf = (struct can_frame *)skb->data;

    id = cf->can_id;
    dlc = CAN_FRAME_LEN(cf);
    data = cf->data;
    if (dlc > DATA_LEN)
      dlc = DATA_LEN;
  }

  tf.rtr = (id & CAN_RTR_FLAG) ? 1 : 0;

  if (id & CAN_EFF_FLAG)
  {
    tf.id_type = 1;
    id &= CAN_EFF_MASK;
  }
  else
  {
    tf.id_type = 0;
    id &= CAN_SFF_MASK;
  }

  for (i = ID_LEN - 1; i >= 0; i--)
  {
    tf.id[i] = id & 0xff;
    id >>= 8;
  }

  tf.dlc = dlc;
  memcpy(tf.data, data, dlc);

  len = TripleSendHex(&tf);

  if (len > max_wire_len)
  {
    /* The firmware cannot take this; sending it wedges the adapter. */
    adapter->devs[channel]->stats.tx_dropped++;
    if (net_ratelimit())
      netdev_warn(adapter->devs[channel], "frame too long for adapter (%d > %d bytes on the wire), dropped\n", len, max_wire_len);
    return -EMSGSIZE;
  }

  if (show_debug_tran)
  {
    printk(KERN_DEBUG "triple: tx port %d len %d:", tf.CAN_port, len);
    for (i = 0; i < len; i++)
      printk(KERN_CONT " %02X", tf.comm_buf[i]);
    printk(KERN_CONT "\n");
  }

  memcpy(adapter->xbuff, tf.comm_buf, len);

  /* Order of next two lines is *very* important.
   * When we are sending a little amount of data,
   * the transfer may be completed inside the ops->write()
   * routine, because it is running with interrupts enabled.
   * In this case we *never* get a WRITE_WAKEUP event
   * if we did not request it before the write operation.
   *       14 Oct 1994  Dmitry Gorodchanin.
   */
  set_bit(TTY_DO_WRITE_WAKEUP, &adapter->tty->flags);
  actual = adapter->tty->ops->write(adapter->tty, adapter->xbuff, len);
  if (actual < 0)
    actual = 0;

  adapter->xleft = len - actual;
  adapter->xhead = adapter->xbuff + actual;
  adapter->current_channel = channel;
  adapter->devs[channel]->stats.tx_bytes += dlc;
  /* Fully handed to the tty in one go: count it here. Otherwise
   * triple_transmit() counts it when the remainder has drained. */
  if (adapter->xleft == 0)
    adapter->devs[channel]->stats.tx_packets++;

  return 0;

} /* END: triple_encaps() */

/*-----------------------------------------------------------------------*/
/* Work item: push the remaining bytes of the current frame to the tty and
 * wake the queues once it has fully drained.
 */
void triple_transmit(struct work_struct *work)
{
  /*=======================================================*/
  print_func_trace(trace_func_tran, __LINE__, __FUNCTION__);
  /*=======================================================*/

  int actual;
  int i;
  USB2CAN_TRIPLE *adapter = container_of(work, USB2CAN_TRIPLE, tx_work);

  spin_lock_bh(&adapter->lock);

  /* First make sure we are connected. */
  if (!adapter->tty || adapter->magic != TRIPLE_MAGIC ||
      (!netif_running(adapter->devs[0]) && !netif_running(adapter->devs[1]) && !netif_running(adapter->devs[2])))
  {
    spin_unlock_bh(&adapter->lock);
    return;
  }

  if (adapter->xleft <= 0)
  {
    /* Nothing left to write: let the next frame in. */
    clear_bit(TTY_DO_WRITE_WAKEUP, &adapter->tty->flags);
    spin_unlock_bh(&adapter->lock);

    for (i = 0; i < 3; i++)
      if (netif_running(adapter->devs[i]))
        netif_wake_queue(adapter->devs[i]);
    return;
  }

  actual = adapter->tty->ops->write(adapter->tty, adapter->xhead, adapter->xleft);
  if (actual < 0)
    actual = 0;
  adapter->xleft -= actual;
  adapter->xhead += actual;
  if (adapter->xleft == 0)
    adapter->devs[adapter->current_channel]->stats.tx_packets++;
  spin_unlock_bh(&adapter->lock);

} /* END: triple_transmit() */
