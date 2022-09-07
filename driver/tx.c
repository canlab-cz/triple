#include <linux/skbuff.h>
#include <linux/version.h>
#include <linux/tty.h>
#include <linux/mutex.h>

#include "tx.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
#include <linux/can/skb.h>
#endif

extern bool trace_func_tran;
extern bool show_debug_tran;
extern void print_func_trace(bool is_trace, int line, const char *func);

void triple_unesc(USB2CAN_TRIPLE *adapter, unsigned char s)
{
  /*=======================================================*/
  // print_func_trace(trace_func_tran, __LINE__, __FUNCTION__);
  /*=======================================================*/

  if (!test_bit(SLF_ERROR, &adapter->flags))
  {
    if (adapter->rcount < TRIPLE_MTU)
    {
      adapter->rbuff[adapter->rcount] = s;
    }
    else
    {
      adapter->devs[0]->stats.rx_over_errors++;
      adapter->devs[1]->stats.rx_over_errors++;
      adapter->devs[2]->stats.rx_over_errors++;
      set_bit(SLF_ERROR, &adapter->flags);
    }
  }
  int tmp_count = adapter->rcount;

  adapter->rcount++;

  if ((tmp_count - 1 > 0) && (adapter->rbuff[tmp_count - 1] != U2C_TR_SPEC_BYTE) && (s == U2C_TR_LAST_BYTE))
  {
    triple_bump(adapter);
    adapter->rcount = 0;
  }

} /* END: triple_unesc() */

/*-----------------------------------------------------------------------*/
// Triple HW (ttyRead) -> Decoder (CMD_TX_CAN)-> SockatCAN message
// recieved message from HW put through decoder if Incoming message push into sockatCAN message a set rx flag on netdev)
void triple_bump(USB2CAN_TRIPLE *adapter)
{
  /*=======================================================*/
  // print_func_trace(trace_func_tran, __LINE__, __FUNCTION__);
  /*=======================================================*/

  TRIPLE_CAN_FRAME frame;
  int i = 0;
  struct sk_buff *skb;
  struct can_frame cf;
  struct canfd_frame cf_fd;

  memset(&frame, 0, sizeof(frame));
  escape_memcpy(frame.comm_buf, adapter->rbuff, adapter->rcount);

  unsigned char *p = frame.comm_buf;

  int ret = 0;
  if ((ret = TripleRecvHex(&frame)) < 0)
  {
    if (show_debug_tran)
      printk("triple : bump : parse fail %d.\n", ret);
    return;
  }

  if (ret == 1)
  {
    if (show_debug_tran)
      ; // printk("U2C_TR_CMD_STATUS\n");
    return;
  }
  else if (ret == 2)
  {
    if (show_debug_tran)
      printk("U2C_TR_CMD_FW_VER\n");
    return;
  }
  else
  {
    if (show_debug_tran)
    {
      for (i = 0; i < COM_BUF_LEN; i++)
        printk("%02X ", *(p + i));
      printk("\n");
    }
  }
  /*if (show_debug_tran)
  {
    for (i = 0; i < COM_BUF_LEN; i++)
      printk("%02X ", *(p + i));
    printk("\n");
  }*/
  if (!frame.fd)
  {
    /*===============================*/
    static int cnt = 1;
    if (show_debug_tran)
    {
      printk("%d. Data: ", cnt++);
      for (i = 0; i < DATA_LEN; i++)
        printk("%02X ", frame.data[i]);
      printk("| ");
      printk("ID: ");
      for (i = 0; i < ID_LEN; i++)
        printk("%02X ", frame.id[i]);
      printk("\n");
      /*===============================*/
    }

    frame.CAN_port = frame.CAN_port; //+ 1;
    frame.id_type = frame.id_type - 1;

    cf.can_id = 0;

    if (frame.rtr)
      cf.can_id |= CAN_RTR_FLAG;

    if (frame.id_type)
      cf.can_id |= CAN_EFF_FLAG;

    for (i = 0; i < ID_LEN; i++)
      cf.can_id |= (frame.id[ID_LEN - 1 - i] << (i * 8));

    /* RTR frames may have a dlc > 0 but they never have any data bytes */
    //*(u64 *)(&cf.data) = 0;

    if (!frame.rtr)
    {
      cf.can_dlc = frame.dlc;
      for (i = 0; i < DATA_LEN; i++)
        cf.data[i] = frame.data[i];
    }
    else
      cf.can_dlc = 0;
  }
  else
  {
    /*===============================*/
    static int cnt = 1;
    if (show_debug_tran)
    {
      printk("%d. Data: ", cnt++);
      for (i = 0; i < DATA_FD_LEN; i++)
        printk("%02X ", frame.data[i]);
      printk("| ");
      printk("ID: ");
      for (i = 0; i < ID_LEN; i++)
        printk("%02X ", frame.id[i]);
      printk("\n");
      /*===============================*/
    }

    frame.CAN_port = frame.CAN_port; //+ 1;
    frame.id_type = frame.id_type - 1;

    cf_fd.can_id = 0;

    if (frame.rtr)
      cf_fd.can_id |= CAN_RTR_FLAG;

    if (frame.id_type)
      cf_fd.can_id |= CAN_EFF_FLAG;

    if (frame.fd_br_switch)
    {
      cf_fd.flags |= CANFD_BRS;
    }

    if (frame.fd_esi)
    {
      cf_fd.flags |= CANFD_ESI;
    }

    for (i = 0; i < ID_LEN; i++)
      cf_fd.can_id |= (frame.id[ID_LEN - 1 - i] << (i * 8));

    /* RTR frames may have a dlc > 0 but they never have any data bytes */
    //*(u64 *)(&cf.data) = 0;

    if (!frame.rtr)
    {
      cf_fd.len = frame.dlc;
      for (i = 0; i < DATA_FD_LEN; i++)
        cf_fd.data[i] = frame.data[i];
    }
    else
      cf_fd.len = 0;
  }

  //---------------------------------------------------------------------------------------------------------
  if (!frame.fd)
  {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
    skb = dev_alloc_skb(sizeof(struct can_frame) + sizeof(struct can_skb_priv));
#else
    skb = dev_alloc_skb(sizeof(struct can_frame));
#endif
  }
  else
  {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
    skb = dev_alloc_skb(sizeof(struct canfd_frame) + sizeof(struct can_skb_priv));
#else
    skb = dev_alloc_skb(sizeof(struct canfd_frame));
#endif
  }

  if (!skb)
  {
    return;
  }

  if (!frame.fd)
    skb->protocol = htons(ETH_P_CAN);
  else
    skb->protocol = htons(ETH_P_CANFD);

  skb->dev = adapter->devs[frame.CAN_port];
  skb->pkt_type = PACKET_BROADCAST;
  skb->ip_summed = CHECKSUM_UNNECESSARY;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
  can_skb_reserve(skb);
  can_skb_prv(skb)->ifindex = adapter->devs[frame.CAN_port]->ifindex;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 5)
  can_skb_prv(skb)->skbcnt = 0;
#endif

  if (!frame.fd)
    memcpy(skb_put(skb, sizeof(struct can_frame)), &cf, sizeof(struct can_frame));
  else
    memcpy(skb_put(skb, sizeof(struct canfd_frame)), &cf_fd, sizeof(struct canfd_frame));

  adapter->devs[frame.CAN_port]->stats.rx_packets++;
  adapter->devs[frame.CAN_port]->stats.rx_bytes += cf.can_dlc;

  netif_rx_ni(skb);

} /* END: triple_bump() */

/*-----------------------------------------------------------------------*/
// sockatCAN frame -> Triple HW (ttyWrite)
void triple_encaps(USB2CAN_TRIPLE *adapter, int channel, struct can_frame *cf)
{
  /*=======================================================*/
  print_func_trace(trace_func_tran, __LINE__, __FUNCTION__);
  /*=======================================================*/

  int i;
  int len = 11;
  int actual;
  canid_t id = cf->can_id;
  TRIPLE_CAN_FRAME triple_frame;

  memset(&triple_frame, 0, sizeof(TRIPLE_CAN_FRAME));

  triple_frame.CAN_port = channel + 1;

  triple_frame.rtr = (cf->can_id & CAN_RTR_FLAG) ? 1 : 0;

  if (cf->can_id & CAN_EFF_FLAG)
  {
    triple_frame.id_type = 1;
    id &= cf->can_id & CAN_EFF_MASK;
  }
  else
  {
    triple_frame.id_type = 0;
    id &= cf->can_id & CAN_SFF_MASK;
  }

  for (i = ID_LEN - 1; i >= 0; i--)
  {
    triple_frame.id[i] = id & 0xff;
    id >>= 8;
  }

  triple_frame.dlc = cf->can_dlc;

  for (i = 0; i < cf->can_dlc; i++)
    triple_frame.data[i] = cf->data[i];

  len = TripleSendHex(&triple_frame);

  /*===============================*/
  if (show_debug_tran)
  {
    printk("CAN_port %d ", triple_frame.CAN_port);

    for (i = 0; i < COM_BUF_LEN; i++)
      printk("%02X ", triple_frame.comm_buf[i]);
    printk("\n");
  }
  memcpy(adapter->xbuff, triple_frame.comm_buf, len);

  /* Order of next two lines is *very* important.
   * When we are sending a little amount of data,
   * the transfer may be completed inside the ops->write()
   * routine, because it's running with interrupts enabled.
   * In this case we *never* got WRITE_WAKEUP event,
   * if we did not request it before write operation.
   *       14 Oct 1994  Dmitry Gorodchanin.
   */

  set_bit(TTY_DO_WRITE_WAKEUP, &adapter->tty->flags);
  actual = adapter->tty->ops->write(adapter->tty, adapter->xbuff, len);

  adapter->xleft = len - actual;
  adapter->xhead = adapter->xbuff + actual;
  adapter->devs[channel]->stats.tx_bytes += cf->can_dlc;

} /* END: triple_encaps() */

// sockatCAN frame -> Triple HW (ttyWrite)
void triple_encaps_fd(USB2CAN_TRIPLE *adapter, int channel, struct canfd_frame *cf)
{
  /*=======================================================*/
  print_func_trace(trace_func_tran, __LINE__, __FUNCTION__);
  /*=======================================================*/

  int i;
  int len = 11;
  int actual;
  canid_t id = cf->can_id;
  TRIPLE_CAN_FRAME triple_frame;

  memset(&triple_frame, 0, sizeof(TRIPLE_CAN_FRAME));

  triple_frame.fd = true;
  triple_frame.CAN_port = channel + 1;

  // RRS insted of RTR, same flag in linux ??
  triple_frame.rtr = (cf->can_id & CAN_RTR_FLAG) ? 1 : 0;

  if (cf->can_id & CAN_EFF_FLAG)
  {
    triple_frame.id_type = 1;
    id &= cf->can_id & CAN_EFF_MASK;
  }
  else
  {
    triple_frame.id_type = 0;
    id &= cf->can_id & CAN_SFF_MASK;
  }

  if (cf->flags & CANFD_BRS)
  {
    triple_frame.fd_br_switch = true;
  }

  if (cf->flags & CANFD_ESI)
  {
    triple_frame.fd_esi = true;
  }

  for (i = ID_LEN - 1; i >= 0; i--)
  {
    triple_frame.id[i] = id & 0xff;
    id >>= 8;
  }

  triple_frame.dlc = cf->len;

  for (i = 0; i < cf->len; i++)
    triple_frame.data[i] = cf->data[i];

  len = TripleSendHex(&triple_frame);
  memcpy(adapter->xbuff, triple_frame.comm_buf, len);

  if (show_debug_tran)
  {
    for (i = 0; i < COM_BUF_LEN; i++)
      printk("%02X ", *(triple_frame.comm_buf + i));
    printk("GREP#1\n");
  }

  set_bit(TTY_DO_WRITE_WAKEUP, &adapter->tty->flags);
  actual = adapter->tty->ops->write(adapter->tty, adapter->xbuff, len);

  adapter->xleft = len - actual;
  adapter->xhead = adapter->xbuff + actual;
  adapter->devs[channel]->stats.tx_bytes += cf->len;

} /* END: triple_encaps() */

// swhatever -> Triple HW (ttyWrite)
void triple_transmit(struct work_struct *work)
{
  /*=======================================================*/
  print_func_trace(trace_func_tran, __LINE__, __FUNCTION__);
  /*=======================================================*/

  int actual;
  USB2CAN_TRIPLE *adapter = container_of(work, USB2CAN_TRIPLE, tx_work);

  spin_lock_bh(&adapter->lock);

  /* First make sure we're connected. */
  if (!adapter->tty || adapter->magic != TRIPLE_MAGIC || (!netif_running(adapter->devs[0]) && !netif_running(adapter->devs[1]) && !netif_running(adapter->devs[2])))
  {
    spin_unlock_bh(&adapter->lock);
    return;
  }

  if (adapter->xleft <= 0)
  {
    adapter->devs[adapter->current_channel]->stats.tx_packets++;

    clear_bit(TTY_DO_WRITE_WAKEUP, &adapter->tty->flags);
    spin_unlock_bh(&adapter->lock);

    if (netif_running(adapter->devs[0]))
      netif_wake_queue(adapter->devs[0]);
    if (netif_running(adapter->devs[1]))
      netif_wake_queue(adapter->devs[1]);
    if (netif_running(adapter->devs[2]))
      netif_wake_queue(adapter->devs[2]);
    return;
  }

  actual = adapter->tty->ops->write(adapter->tty, adapter->xhead, adapter->xleft);
  adapter->xleft -= actual;
  adapter->xhead += actual;
  spin_unlock_bh(&adapter->lock);

} /* END: triple_transmit() */
