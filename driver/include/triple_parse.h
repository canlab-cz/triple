#ifndef __TRIPLE_PARSE_H__
#define __TRIPLE_PARSE_H__

#include "triple_helper.h"

/*--------------------------------------*/
/* Encode frame->... into frame->comm_buf, returns encoded length. */
int TripleSendHex(TRIPLE_CAN_FRAME *frame);

/*
 * Decode an unescaped frame of 'len' bytes from frame->comm_buf.
 * Returns 0 for a CAN frame, 1 for a status reply, 2 for a firmware
 * version reply, and a negative value for anything that is malformed
 * or not (yet) supported.
 */
int TripleRecvHex(TRIPLE_CAN_FRAME *frame, size_t len);

#endif
