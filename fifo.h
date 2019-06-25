#ifndef FQUE_H_20170118
#define FQUE_H_20170118

#include "ncb.h"

struct tx_node {
    unsigned char *data; /* data buffer for Tx */
    int wcb; /* the total count of bytes need to write */
    int offset; /* the current offset of success written */
    struct sockaddr_in udp_target; /* the Tx target address, UDP only */
    struct sockaddr_ll arp_target; /* the Tx target address, ARP only */
    struct list_head link;
};

extern void fifo_init(ncb_t *ncb);
extern void fifo_uninit(ncb_t *ncb);

/* queue Tx node into pending fifo.
 * the io blocking status will be automatic recover to 1 */
extern int fifo_queue(ncb_t *ncb, struct tx_node *node);

/* get front item of current fifo */
extern int fifo_top(ncb_t *ncb, struct tx_node **node);

/* pop the front item from current fifo, option, you can get the top one
 * when the last item have been pop from the fifo, the io blocking status will be automatic recover to 0,
 * if @node is null, the front item memory will be free after pop, otherwise, the calling thread MUST free @*node after @fifo_pop return 1
 *
 * the return value definition:
 * -1 : error occurred
 * 0  : the queue is empty
 * 1  : success pop top item
 */
extern int fifo_pop(ncb_t *ncb, struct tx_node **node);

/* test the current io block status, boolean return */
extern int fifo_is_blocking(ncb_t *ncb);

#endif
