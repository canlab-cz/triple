#ifndef __TRANSCEIVE_H__
#define __TRANSCEIVE_H__

#include <linux/version.h>
#include <linux/can.h>
#include <linux/workqueue.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>

#include "triple_helper.h"
#include "triple_parse.h"

/* struct can_frame.can_dlc was renamed to len in 5.11 (old name is deprecated) */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#define CAN_FRAME_LEN(cf) ((cf)->len)
#else
#define CAN_FRAME_LEN(cf) ((cf)->can_dlc)
#endif

void triple_unesc   (USB2CAN_TRIPLE *adapter, unsigned char s);
int  triple_encaps  (USB2CAN_TRIPLE *adapter, int channel, struct sk_buff *skb);
void triple_transmit(struct work_struct *work);

#endif
