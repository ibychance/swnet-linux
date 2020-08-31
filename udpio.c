#include "udp.h"

#include "mxx.h"
#include "fifo.h"

static
int __udp_rx(ncb_t *ncb)
{
    int recvcb;
    struct sockaddr_in remote;
    socklen_t addrlen;
    udp_data_t c_data;
    nis_event_t c_event;

    addrlen = sizeof (struct sockaddr_in);
    recvcb = recvfrom(ncb->sockfd, ncb->packet, MAX_UDP_UNIT, 0, (struct sockaddr *) &remote, &addrlen);
    if (recvcb > 0) {
        c_event.Ln.Udp.Link = ncb->hld;
        c_event.Event = EVT_RECEIVEDATA;
        c_data.e.Packet.Data = ncb->packet;
        c_data.e.Packet.Size = recvcb;
        inet_ntop(AF_INET, &remote.sin_addr, c_data.e.Packet.RemoteAddress, sizeof (c_data.e.Packet.RemoteAddress));
        c_data.e.Packet.RemotePort = ntohs(remote.sin_port);
        if (ncb->nis_callback) {
            ncb->nis_callback(&c_event, &c_data);
        }
    }

    if (0 == recvcb) {
        nis_call_ecr("[nshost.udpio.__udp_rx] fatal error occurred syscall recvfrom(2),the return value equal to zero,link:%lld", ncb->hld);
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

        nis_call_ecr("[nshost.udpio.__udp_rx] fatal error occurred syscall recvfrom(2), error:%d, link:%lld", errno, ncb->hld );
        return -1;
    }

    return 0;
}

int udp_rx(ncb_t *ncb)
{
     int retval;

    do {
        retval = __udp_rx(ncb);
    } while (0 == retval);

    return retval;
}

int udp_txn(ncb_t *ncb, void *p)
{
    int wcb;
    struct tx_node *node;
    socklen_t len;

	node = (struct tx_node *)p;
	if (!node) {
		return -EINVAL;
	}

    while (node->offset < node->wcb) {
		len = sizeof(struct sockaddr);
        wcb = sendto(ncb->sockfd, node->data + node->offset, node->wcb - node->offset, 0,
                (const struct sockaddr *)&node->udp_target, len );

        /* fatal-error/connection-terminated  */
        if (0 == wcb) {
            nis_call_ecr("[nshost.udpio.udp_txn] fatal error occurred syscall sendto(2), the return value equal to zero, link:%lld", ncb->hld);
            return -1;
        }

        if (wcb < 0) {
            /* the write buffer is full, active EPOLLOUT and waitting for epoll event trigger
             * at this point, we need to deal with the queue header node and restore the unprocessed node back to the queue header.
             * the way 'oneshot' focus on the write operation completion point */
            if (EAGAIN == errno) {
                nis_call_ecr("[nshost.udpio.udp_txn] syscall sendto(2) would block cause by kernel memory overload,link:%lld", ncb->hld);
                return -EAGAIN;
            }

            /* A signal occurred before any data  was  transmitted
                continue and send again */
            if (EINTR == errno) {
                continue;
            }

             /* other error, these errors should cause link close */
            nis_call_ecr("[nshost.udpio.udp_txn] fatal error occurred syscall sendto(2), error:%d, link:%lld",errno, ncb->hld );
            return -1;
        }

        node->offset += wcb;
    }

    return node->wcb;
}

int udp_tx(ncb_t *ncb)
{
    struct tx_node *node;
    int retval;

    if (!ncb) {
        return -1;
    }

    /* try to write front package into system kernel send-buffer */
    if (fifo_top(ncb, &node) >= 0) {
        retval = udp_txn(ncb, node);
        if (retval > 0) {
            fifo_pop(ncb, NULL);
        }
        return retval;
    }

    return 0;
}
