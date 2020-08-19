#include "arp.h"

#include "mxx.h"
#include "fifo.h"
#include "io.h"
#include "wpool.h"

static
int __arp_rx(ncb_t *ncb)
{
    int recvcb;
    struct sockaddr_ll remote;
    socklen_t addrlen;
    arp_data_t c_data;
    nis_event_t c_event;
    struct Ethernet_Head  *eth;
    struct Address_Resolution_Protocol *arp;

    addrlen = sizeof(remote);
    recvcb = recvfrom(ncb->sockfd, ncb->packet, NIS_P_ARP_SIZE, 0, (struct sockaddr *) &remote, &addrlen);
    if (recvcb > 0) {
    	if (recvcb < NIS_P_ARP_SIZE) {
    		return 0;
    	}
        eth = (struct Ethernet_Head  *)ncb->packet;
        arp = (struct Address_Resolution_Protocol *)&ncb->packet[sizeof(struct Ethernet_Head)];
        if (eth->Eth_Layer_Type == htons(ETH_P_ARP) && arp->Arp_Op_Code == htons(ARP_OP_REPLY) && ncb->nis_callback) {
            c_event.Ln.Udp.Link = ncb->hld;
            c_event.Event = EVT_RECEIVEDATA;
            memcpy(&c_data.e.Packet, arp, sizeof(struct Address_Resolution_Protocol));

            /* keep little-endian, keep compatibility */
            c_data.e.Packet.Arp_Sender_Ip = ntohl(c_data.e.Packet.Arp_Sender_Ip);
            c_data.e.Packet.Arp_Target_Ip = ntohl(c_data.e.Packet.Arp_Target_Ip);

            /* callback to arp reply */
            ncb->nis_callback(&c_event, &c_data);
        }
    }

    if (0 == recvcb) {
        nis_call_ecr("[nshost.udpio.__arp_rx] fatal error occurred syscall recvfrom(2),the return value equal to zero,link:%lld", ncb->hld);
        return -1;
    }

    /* ECONNRESET 104 Connection reset by peer */
    if (recvcb < 0){
        if ((EAGAIN == errno) || (EWOULDBLOCK == errno)){
            return EAGAIN;
        }

        /* system interrupted */
        if (EINTR == errno) {
            return 0;
        }

        nis_call_ecr("[nshost.udpio.__arp_rx] fatal error occurred syscall recvfrom(2), error:%d, link:%lld", errno, ncb->hld );
        return -1;
    }

    return 0;
}

int arp_rx(ncb_t *ncb)
{
     int retval;

    do {
        retval = __arp_rx(ncb);
    } while (0 == retval);

    return retval;
}

int arp_txn(ncb_t *ncb, void *p)
{
    int wcb;
    struct tx_node *node;

	node = (struct tx_node *)p;
	if (!node) {
		return -EINVAL;
	}

    while (node->offset < node->wcb) {
        wcb = sendto(ncb->sockfd, node->data + node->offset, node->wcb - node->offset, 0,
                (const struct sockaddr *)&node->arp_target, sizeof(node->arp_target) );

        /* fatal-error/connection-terminated  */
        if (0 == wcb) {
            nis_call_ecr("[nshost.arpio.arp_txn] fatal error occurred syscall sendto(2), the return value equal to zero, link:%lld", ncb->hld);
            return -1;
        }

        if (wcb < 0) {
            /* the write buffer is full, active EPOLLOUT and waitting for epoll event trigger
             * at this point, we need to deal with the queue header node and restore the unprocessed node back to the queue header.
             * the way 'oneshot' focus on the write operation completion point */
            if (EAGAIN == errno) {
                nis_call_ecr("[nshost.arpio.arp_txn] syscall sendto(2) would block cause by kernel memory overload,link:%lld", ncb->hld);
                return -EAGAIN;
            }

            /* A signal occurred before any data  was  transmitted
                continue and send again */
            if (EINTR == errno) {
                continue;
            }

             /* other error, these errors should cause link close */
            nis_call_ecr("[nshost.arpio.arp_txn] fatal error occurred syscall sendto(2), error:%d, link:%lld",errno, ncb->hld );
            return -1;
        }

        node->offset += wcb;
    }

    return node->wcb;
}

int arp_tx(ncb_t *ncb)
{
    struct tx_node *node;
    int retval;

    if (!ncb) {
        return -1;
    }

    /* try to write front package into system kernel send-buffer */
    if (fifo_top(ncb, &node) >= 0) {
        retval = arp_txn(ncb, node);
        if (retval > 0) {
            fifo_pop(ncb, NULL);
        }
        return retval;
    }

    return 0;
}
