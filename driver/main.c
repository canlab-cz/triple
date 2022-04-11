/*
 * Canlab USB2CAN Triple interface driver (using tty line discipline)
 *
 * This file is derived from linux/drivers/net/slip/slip.c
 * and from Innodisk EMUC-B202 CAN driver
 *
 * slip.c Authors  : Laurence Culhane <loz@holmes.demon.co.uk>
 *                   Fred N. van Kempen <waltje@uwalt.nl.mugnet.org>
 * slcan.c Author  : Oliver Hartkopp <socketcan@hartkopp.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307. You can also get it
 * at http://www.gnu.org/licenses/gpl.html
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/version.h>
#include <linux/rtnetlink.h>
#include <linux/if_arp.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
#include <linux/can/dev.h>
#endif
#include "tx.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TripleCAN interface driver");
MODULE_ALIAS("USB2CAN Triple");
MODULE_VERSION("v1.3");
MODULE_AUTHOR("Canlab");

/* global variables & define */
#define N_TRIPLE (NR_LDISCS - 1)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
/* pcan_netdev_register() use alloc_candev() instead of alloc_netdev() */
#define USES_ALLOC_CANDEV

/* using alloc_candev() also means:
 * - don't care about LINUX_CAN_RESTART_TIMER (restart is handled by can_restart_work)
 */
#endif

bool trace_func_main = true;
bool trace_func_tran = true;
bool trace_func_pars = true;
bool show_debug_main = true;
bool show_debug_tran = true;
bool show_debug_pars = true;

int maxdev = 3;
__initconst const char banner[] = "USB2CAN TRIPLE SocketCAN interface driver\n";
struct net_device **triple_devs;

/* driver layer - (1) Kernel module basics */
static int __init triple_init(void);
static void __exit triple_exit(void);

module_init(triple_init);
module_exit(triple_exit);

/* driver layer - (2)  TTY line discipline */
static int triple_open(struct tty_struct *tty);
static void triple_close(struct tty_struct *tty);
static int triple_hangup(struct tty_struct *tty);
static int triple_ioctl(struct tty_struct *tty, struct file *file, unsigned int cmd, unsigned long arg);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
static void triple_receive_buf(struct tty_struct *tty, const unsigned char *cp, const char *fp, int count);
#else
static void triple_receive_buf(struct tty_struct *tty, const unsigned char *cp, char *fp, int count);
#endif
static void triple_write_wakeup(struct tty_struct *tty);

static struct tty_ldisc_ops triple_ldisc =
    {
        .owner = THIS_MODULE,
        .num = N_TRIPLE,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 13, 0)
        .magic = TTY_LDISC_MAGIC,
#endif
        .name = "triplecan",
        .open = triple_open,
        .close = triple_close,
        .hangup = triple_hangup,
        .ioctl = triple_ioctl,
        .receive_buf = triple_receive_buf,
        .write_wakeup = triple_write_wakeup,
};

/* driver layer - (3) Network layer*/
static int triple_netdev_open(struct net_device *dev);
static int triple_netdev_close(struct net_device *dev);
static netdev_tx_t triple_xmit(struct sk_buff *skb, struct net_device *dev);
static int triple_change_mtu(struct net_device *dev, int new_mtu);

static struct net_device_ops triple_netdev_ops =
    {
        .ndo_open = triple_netdev_open,
        .ndo_stop = triple_netdev_close,
        .ndo_start_xmit = triple_xmit,
        .ndo_change_mtu = triple_change_mtu,
};

/* internal function */
static void triple_sync(void);
static int triple_alloc(dev_t line, USB2CAN_TRIPLE *adapter);
static void triple_setup(struct net_device *dev);
static void triple_fd_setup(struct net_device *dev);
static void triple_free_netdev(struct net_device *dev);

void print_func_trace(bool is_trace, int line, const char *func);

static int __init triple_init(void)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  int status;

  if (maxdev < 6)
    maxdev = 6; /* Sanity */

  printk(banner);
  printk(KERN_ERR "triple: %d interface channels.\n", maxdev);

  triple_devs = kzalloc(sizeof(struct net_device *) * maxdev, GFP_KERNEL);

  if (!triple_devs)
    return -ENOMEM;

/* Fill in our line protocol discipline, and register it */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
  status = tty_register_ldisc(&triple_ldisc);
#else
  status = tty_register_ldisc(N_TRIPLE, &triple_ldisc);
#endif

  printk(KERN_ERR "triple: register line discipline%d\n", N_TRIPLE);

  if (status)
  {
    printk(KERN_ERR "triple: can't register line discipline\n");
    kfree(triple_devs);
  }

  return status;

} /* END: triple_init() */

static void __exit triple_exit(void)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  int i = 0;
  int busy = 0;
  struct net_device *dev;
  USB2CAN_TRIPLE *adapter;
  unsigned long timeout = jiffies + HZ;

  if (triple_devs == NULL)
    return;

  /* First of all: check for active disciplines and hangup them. */
  do
  {
    if (busy)
      msleep_interruptible(100);

    busy = 0;

    for (i = 0; i < maxdev; i++)
    {
      dev = triple_devs[i];

      if (!dev)
        continue;

      adapter = ((TRIPLE_PRIV *)netdev_priv(dev))->adapter;

      spin_lock_bh(&adapter->lock);
      if (adapter->tty)
      {
        busy++;
        tty_hangup(adapter->tty);
      }
      spin_unlock_bh(&adapter->lock);
    }

  } while (busy && time_before(jiffies, timeout));

  /* FIXME: hangup is async so we should wait when doing this second phase */

  for (i = 0; i < maxdev; i++)
  {
    dev = triple_devs[i];

    if (!dev)
      continue;

    triple_devs[i] = NULL;

    adapter = ((TRIPLE_PRIV *)netdev_priv(dev))->adapter;

    if (adapter->tty)
    {
      printk(KERN_ERR "%s: tty discipline still running\n", dev->name);

      /* Intentionally leak the control block. */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 9)
      dev->priv_destructor = NULL;
#else
      dev->destructor = NULL;
#endif
    }
    unregister_netdev(dev);
  }

  kfree(triple_devs);
  triple_devs = NULL;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
  tty_unregister_ldisc(&triple_ldisc);
#else
  tty_unregister_ldisc(N_TRIPLE);
#endif

} /* END: triple_exit() */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
static void triple_receive_buf(struct tty_struct *tty, const unsigned char *cp, const char *fp, int count)
#else
static void triple_receive_buf(struct tty_struct *tty, const unsigned char *cp, char *fp, int count)
#endif
{
  USB2CAN_TRIPLE *adapter = (USB2CAN_TRIPLE *)tty->disc_data;

  if (!adapter || adapter->magic != TRIPLE_MAGIC || (!netif_running(adapter->devs[0]) && !netif_running(adapter->devs[1]) && !netif_running(adapter->devs[2])))
    return;

  /* Read the characters out of the buffer */
  while (count--)
  {
    if (fp && *fp++)
    {
      if (!test_and_set_bit(SLF_ERROR, &adapter->flags))
      {
        if (netif_running(adapter->devs[0]))
          adapter->devs[0]->stats.rx_errors++;

        if (netif_running(adapter->devs[1]))
          adapter->devs[1]->stats.rx_errors++;

        if (netif_running(adapter->devs[2]))
          adapter->devs[2]->stats.rx_errors++;
      }

      cp++;
      continue;
    }
    triple_unesc(adapter, *cp++);
  }

} /* END: triple_receive_buf() */

static int triple_open(struct tty_struct *tty)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  int err;
  USB2CAN_TRIPLE *adapter;

  if (!capable(CAP_NET_ADMIN))
    return -EPERM;

  if (tty->ops->write == NULL)
    return -EOPNOTSUPP;

  /* RTnetlink lock is misused here to serialize concurrent
     opens of triple channels. There are better ways, but it is
     the simplest one.
   */
  rtnl_lock();

  /* Collect hanged up channels. */
  triple_sync();

  adapter = tty->disc_data;
  err = -EEXIST;

  /* First make sure we're not already connected. */
  if (adapter && adapter->magic == TRIPLE_MAGIC)
    goto ERR_EXIT;

  /* OK. Allocate triple adapter. */
  err = -ENOMEM;
  adapter = kzalloc(sizeof(*adapter), GFP_KERNEL);
  if (!adapter)
    goto ERR_EXIT;

  /* OK.  Find a free triple channel to use. */
  err = -ENFILE;
  if (triple_alloc(tty_devnum(tty), adapter) != 0)
  {
    kfree(adapter);
    goto ERR_EXIT;
  }

  adapter->tty = tty;
  tty->disc_data = adapter;

  if (!test_bit(SLF_INUSE, &adapter->flags))
  {
    /* Perform the low-level triple initialization. */
    adapter->rcount = 0;
    adapter->xleft = 0;

    set_bit(SLF_INUSE, &adapter->flags);

#ifdef USES_ALLOC_CANDEV
    if (tty->dev)
    {
      SET_NETDEV_DEV(adapter->devs[0], tty->dev);
      SET_NETDEV_DEV(adapter->devs[1], tty->dev);
      SET_NETDEV_DEV(adapter->devs[2], tty->dev);
    }
    else
      goto ERR_EXIT;
#endif /* USES_ALLOC_CANDEV */

    err = register_netdevice(adapter->devs[0]);
    if (err)
      goto ERR_FREE_CHAN;

    err = register_netdevice(adapter->devs[1]);
    if (err)
    {
      unregister_netdev(adapter->devs[0]);
      goto ERR_FREE_CHAN;
    }

    err = register_netdevice(adapter->devs[2]);
    if (err)
    {
      unregister_netdev(adapter->devs[0]);
      unregister_netdev(adapter->devs[1]);
      goto ERR_FREE_CHAN;
    }
  }

  /* Done.  We have linked the TTY line to a channel. */
  rtnl_unlock();
  tty->receive_room = 65536; /* We don't flow control */

  /* TTY layer expects 0 on success */
  return 0;

ERR_FREE_CHAN:
  adapter->tty = NULL;
  tty->disc_data = NULL;
  clear_bit(SLF_INUSE, &adapter->flags);

ERR_EXIT:
  rtnl_unlock();

  /* Count references from TTY module */
  return err;

} /* END: triple_open() */

static void triple_close(struct tty_struct *tty)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  USB2CAN_TRIPLE *adapter = (USB2CAN_TRIPLE *)tty->disc_data;

  /* First make sure we're connected. */
  /*if (!adapter || adapter->magic != TRIPLE_MAGIC || adapter->tty != tty)
    return;
  */
  spin_lock_bh(&adapter->lock);
  tty->disc_data = NULL;
  adapter->tty = NULL;
  spin_unlock_bh(&adapter->lock);

  flush_work(&adapter->tx_work);

  /* Flush network side */
  unregister_netdev(adapter->devs[0]);
  unregister_netdev(adapter->devs[1]);
  unregister_netdev(adapter->devs[2]);
  /* This will complete via triple_free_netdev */

} /* END: triple_close() */

static int triple_hangup(struct tty_struct *tty)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  triple_close(tty);
  return 0;

} /* END: triple_hangup() */

static int triple_ioctl(struct tty_struct *tty, struct file *file, unsigned int cmd, unsigned long arg)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  int channel;
  unsigned int tmp;
  USB2CAN_TRIPLE *adapter = (USB2CAN_TRIPLE *)tty->disc_data;

  /* First make sure we're connected. */
  if (!adapter || adapter->magic != TRIPLE_MAGIC)
    return -EINVAL;

  switch (cmd)
  {
  case SIOCGIFNAME:
  {
    if (adapter->gif_channel > 2)
      return 0;

    printk("gif_channel:%d\n", adapter->gif_channel);

    channel = adapter->gif_channel;

    tmp = strlen(adapter->devs[channel]->name) + 1;
    printk("actual name%s\n", adapter->devs[channel]->name);

    printk("name%s\n", adapter->devs[channel]->name);

    if (copy_to_user((void __user *)arg, adapter->devs[channel]->name, tmp))
      return -EFAULT;

    adapter->gif_channel = adapter->gif_channel + 1 % 3;
    return 0;
  }

  case SIOCSIFHWADDR:
    return -EINVAL;

  default:
    return tty_mode_ioctl(tty, file, cmd, arg);
  }

} /* END: triple_ioctl() */

static void triple_write_wakeup(struct tty_struct *tty)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  USB2CAN_TRIPLE *adapter = tty->disc_data;

  schedule_work(&adapter->tx_work);

} /* END: triple_write_wakeup() */

/******************************************
 *   Routines looking at netdevice side.
 ******************************************/

static int triple_netdev_open(struct net_device *dev)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  USB2CAN_TRIPLE *adapter = ((TRIPLE_PRIV *)netdev_priv(dev))->adapter;

  if (adapter->tty == NULL)
    return -ENODEV;

  adapter->flags &= (1 << SLF_INUSE);
  netif_start_queue(dev);

  return 0;

} /* END: triple_netdev_open() */
static int triple_netdev_close(struct net_device *dev)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  int channel;
  USB2CAN_TRIPLE *adapter = ((TRIPLE_PRIV *)netdev_priv(dev))->adapter;

  channel = (dev->base_addr & 0xF00) >> 8;
  if (channel > 2)
  {
    printk(KERN_WARNING "%s: close: invalid channel\n", dev->name);
    return -1;
  }

  spin_lock_bh(&adapter->lock);

  if (adapter->tty)
  {
    /* TTY discipline is running. */
    if (!netif_running(adapter->devs[!channel]))
      clear_bit(TTY_DO_WRITE_WAKEUP, &adapter->tty->flags);
  }

  netif_stop_queue(dev);

  if (!netif_running(adapter->devs[!channel]))
  {
    /* another netdev is closed (down) too, reset TTY buffers. */
    adapter->rcount = 0;
    adapter->xleft = 0;
  }

  spin_unlock_bh(&adapter->lock);

  return 0;

} /* END: triple_netdev_close() */

static netdev_tx_t triple_xmit(struct sk_buff *skb, struct net_device *dev)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  int channel;
  USB2CAN_TRIPLE *adapter = ((TRIPLE_PRIV *)netdev_priv(dev))->adapter;

  if ((skb->len != sizeof(struct can_frame)) && (skb->len != sizeof(struct canfd_frame)))
    goto OUT;

  spin_lock(&adapter->lock);

  if (!netif_running(dev))
  {
    spin_unlock(&adapter->lock);
    printk(KERN_WARNING "%s: xmit: iface is down\n", dev->name);

    goto OUT;
  }

  if (adapter->tty == NULL)
  {
    spin_unlock(&adapter->lock);
    goto OUT;
  }

  channel = (dev->base_addr & 0xF00) >> 8;

  if (channel > 2)
  {
    spin_unlock(&adapter->lock);
    printk(KERN_WARNING "%s: xmit: invalid channel\n", dev->name);
    goto OUT;
  }

  netif_stop_queue(adapter->devs[0]);
  netif_stop_queue(adapter->devs[1]);
  netif_stop_queue(adapter->devs[2]);

   int can_fd_channel = 2;

  if ((channel == can_fd_channel)) // CAN_FD
  {
  triple_encaps_fd(adapter, 2, (struct canfd_frame *)skb->data); // sockatCAN frame -> Triple HW (ttyWrite)
  }
   else // CAN 2.0
  {
     triple_encaps(adapter, channel, (struct can_frame *)skb->data); // sockatCAN frame -> Triple HW (ttyWrite)
  }

  spin_unlock(&adapter->lock);

OUT:
  kfree_skb(skb);
  return NETDEV_TX_OK;

} /* END: triple_xmit() */

static int triple_change_mtu(struct net_device *dev, int new_mtu)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/
  return -EINVAL;

} /* END: triple_change_mtu() */

static void triple_sync(void)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  int i;
  struct net_device *dev;
  USB2CAN_TRIPLE *adapter;

  for (i = 0; i < maxdev; i++)
  {
    dev = triple_devs[i];

    if (dev == NULL)
      break;

    adapter = ((TRIPLE_PRIV *)netdev_priv(dev))->adapter;

    if (adapter->tty)
      continue;

    if (dev->flags & IFF_UP)
      dev_close(dev);
  }

} /* END: triple_sync() */

static int triple_alloc(dev_t line, USB2CAN_TRIPLE *adapter)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  int i;
  int channel;
  int id[3];
  char name[IFNAMSIZ];
  struct net_device *dev;
  struct net_device *devs[3];
  TRIPLE_PRIV *priv;

  channel = 0;

  for (i = 0; i < maxdev; i++)
  {
    dev = triple_devs[i];

    if (dev == NULL)
    {
      id[channel++] = i;

      if (channel > 2)
        break;
    }
  }

  /* Sorry, too many, all slots in use */
  if (i >= maxdev)
    return -1;

  sprintf(name, "triplecan%d", id[0]);
#ifdef USES_ALLOC_CANDEV
  devs[0] = alloc_candev(sizeof(*priv), 0);
#else
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
  devs[0] = alloc_netdev(sizeof(*priv), name, triple_setup);
#else
  devs[0] = alloc_netdev(sizeof(*priv), name, NET_NAME_UNKNOWN, triple_setup);
#endif
#endif /* USES_ALLOC_CANDEV */

  if (!devs[0])
    return -1;

#ifdef USES_ALLOC_CANDEV
  strncpy(devs[0]->name, name, sizeof(devs[0]->name));
#endif /* USES_ALLOC_CANDEV */
  sprintf(name, "triplecan%d", id[1]);

#ifdef USES_ALLOC_CANDEV
  devs[1] = alloc_candev(sizeof(*priv), 0);
#else
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
  devs[1] = alloc_netdev(sizeof(*priv), name, triple_setup);
#else
  devs[1] = alloc_netdev(sizeof(*priv), name, NET_NAME_UNKNOWN, triple_setup);
#endif
#endif /* USES_ALLOC_CANDEV */

  if (!devs[1])
  {
    free_netdev(devs[0]);
    return -1;
  }
#ifdef USES_ALLOC_CANDEV
  strncpy(devs[1]->name, name, sizeof(devs[1]->name));
#endif /* USES_ALLOC_CANDEV */

  sprintf(name, "triplecan%d", id[2]);

  adapter->can_fd = true;

  if (adapter->can_fd)
  {
    printk("---------------->canfd alloc netdev\n");

#ifdef USES_ALLOC_CANDEV
    devs[2] = alloc_candev(sizeof(*priv), 0);
#else
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
    devs[2] = alloc_netdev(sizeof(*priv), name, triple_fd_setup);
#else
    devs[2] = alloc_netdev(sizeof(*priv), name, NET_NAME_UNKNOWN, triple_fd_setup);
#endif
#endif /* USES_ALLOC_CANDEV */
  }
  else
  {

#ifdef USES_ALLOC_CANDEV
    devs[2] = alloc_candev(sizeof(*priv), 0);
#else
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
    devs[2] = alloc_netdev(sizeof(*priv), name, triple_setup);
#else
    devs[2] = alloc_netdev(sizeof(*priv), name, NET_NAME_UNKNOWN, triple_setup);
#endif
#endif /* USES_ALLOC_CANDEV */
  }

  if (!devs[2])
  {
    free_netdev(devs[0]);
    free_netdev(devs[1]);
    return -1;
  }
#ifdef USES_ALLOC_CANDEV
  strncpy(devs[2]->name, name, sizeof(devs[2]->name));
#endif /* USES_ALLOC_CANDEV */

  devs[0]->base_addr = id[0];
  devs[1]->base_addr = 0x100 | id[1];
  devs[2]->base_addr = 0x200 | id[2];

  priv = netdev_priv(devs[0]);
  priv->magic = TRIPLE_MAGIC;
  priv->adapter = adapter;
  priv = netdev_priv(devs[1]);
  priv->magic = TRIPLE_MAGIC;
  priv->adapter = adapter;
  priv = netdev_priv(devs[2]);
  priv->magic = TRIPLE_MAGIC;
  priv->adapter = adapter;

#ifdef USES_ALLOC_CANDEV
  triple_setup(devs[0]);
  triple_setup(devs[1]);
  // if (adapter->can_fd)
  triple_fd_setup(devs[2]);
  // else
  //   triple_setup(devs[2]);

#endif /* USES_ALLOC_CANDEV */

  /* Initialize channel control data */
  adapter->magic = TRIPLE_MAGIC;
  adapter->devs[0] = devs[0];
  adapter->devs[1] = devs[1];
  adapter->devs[2] = devs[2];
  triple_devs[id[0]] = devs[0];
  triple_devs[id[1]] = devs[1];
  triple_devs[id[2]] = devs[2];
  spin_lock_init(&adapter->lock);
  atomic_set(&adapter->ref_count, 3); //?
  INIT_WORK(&adapter->tx_work, triple_transmit);

  return 0;

} /* END: triple_alloc() */

static void triple_setup(struct net_device *dev)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  dev->netdev_ops = &triple_netdev_ops;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 9)
  dev->priv_destructor = triple_free_netdev;
#else
  dev->destructor = triple_free_netdev;
#endif

  dev->hard_header_len = 0;
  dev->addr_len = 0;
  dev->tx_queue_len = 100;

  dev->mtu = sizeof(struct can_frame); // CAN_MTU;canfd_frame
  dev->type = ARPHRD_CAN;

  /* New-style flags. */
  dev->flags = IFF_NOARP;
  dev->features = NETIF_F_HW_CSUM;

} /* END: triple_setup() */

static void triple_fd_setup(struct net_device *dev)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  dev->netdev_ops = &triple_netdev_ops;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 9)
  dev->priv_destructor = triple_free_netdev;
#else
  dev->destructor = triple_free_netdev;
#endif

  dev->hard_header_len = 0;
  dev->addr_len = 0;
  dev->tx_queue_len = 100;

  dev->mtu = sizeof(struct canfd_frame);
  ;
  dev->type = ARPHRD_CAN;

  /* New-style flags. */
  dev->flags = IFF_NOARP;
  dev->features = NETIF_F_HW_CSUM;

} /* END: triple_setup() */

static void triple_free_netdev(struct net_device *dev)
{
  /*=======================================================*/
  print_func_trace(trace_func_main, __LINE__, __FUNCTION__);
  /*=======================================================*/

  int i = (dev->base_addr & 0xFF);
  USB2CAN_TRIPLE *adapter = ((TRIPLE_PRIV *)netdev_priv(dev))->adapter;

  free_netdev(dev);

  triple_devs[i] = NULL;

  if (atomic_dec_and_test(&adapter->ref_count))
  {
    printk("free_netdev: free adapter\n");
    kfree(adapter);
  }

} /* END: triple_free_netdev() */

/*---------------------------------------------------------------------------------------------------*/
void print_func_trace(bool is_trace, int line, const char *func)
{
  if (is_trace)
    printk("----------------> %d, %s()\n", line, func);

} /* END: print_func_trace() */
