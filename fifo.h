#ifndef FQUE_H_20170118
#define FQUE_H_20170118

#include "ncb.h"

struct tx_node {
    unsigned char *data; /* data buffer for Tx */
    int wcb; /* the total count of bytes need to write */
    int offset; /* the current offset of @data after success written */
    struct sockaddr_in udp_target; /* the Tx target address, UDP only */
    struct sockaddr_ll arp_target; /* the Tx target address, ARP only */
    struct list_head link;
};

extern void fifo_init(ncb_t *ncb);
extern void fifo_uninit(ncb_t *ncb);

/* atomic implememnt get the top node of current fifo,
 *	@node MUST be a effective pointer to retrieve the queued item.
 *	calling thread with responsibility to manage the memory buffer return by *node
 */
extern int fifo_top(ncb_t *ncb, struct tx_node **node);

/* after syscall write(2) or send(2) failed with error EAGAIN, low-level cache of operation system kernel cannot hold more data buffer,
 *	in this situation, application should preserve these data and waitting for the kernel state change to idle.
 *	@fifo_queue be responsible for associated object @ncb with EPOLLOUT event, and storage data buffer @node->data to the tail of fifo queue */
extern int fifo_queue(ncb_t *ncb, struct tx_node *node);

/* atomic implememnt to get the top node of current fifo and then pop it out,
 * 	in this procedure, after certain no any other items in the queue but the IO blocking state are still presences
 *	@fifo_pop be responsible for disassociation object @ncb with EPOLLOUT event.
 *  notes:
 *	1. @node is a option parameter, it canbe null-ptr,
 *		if it is, the memory of top item will be freed after called,
 *		otherwise, calling thread with responsibility to manage the memory buffer return by *node
 *	2. meaning of return value:
 *		return -1: error occurred.
 *		return 0:  the fifo is empty, pop can't be done
 *		return 1:  pop success and *node is the top one */
extern int fifo_pop(ncb_t *ncb, struct tx_node **node);

/* test the fifo blocking state, use boolean predicate for the return value:
 *	return 1: the IO is blocking
 *	return 0: the IO is non-blocking  */
extern int fifo_is_blocking(ncb_t *ncb);

#endif
