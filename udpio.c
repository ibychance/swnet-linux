#include "udp.h"

#include "mxx.h"
#include "fifo.h"

static
int __udp_rx(ncb_t *ncb) {
    int recvcb;
    struct sockaddr_in remote;
    socklen_t addrlen;
    udp_data_t c_data;
    nis_event_t c_event;
    int errcode;

    addrlen = sizeof (struct sockaddr_in);
    recvcb = recvfrom(ncb->sockfd, ncb->packet, MAX_UDP_SIZE, 0, (struct sockaddr *) &remote, &addrlen);
    errcode = errno;
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
        if ((EAGAIN == errcode) || (EWOULDBLOCK == errcode)){
            return EAGAIN;
        }
        
        /* system interrupted */
        if (EINTR == errcode) {
            return 0;
        }

        nis_call_ecr("[nshost.udpio.__udp_rx] fatal error occurred syscall recvfrom(2), error:%d, link:%lld", errcode, ncb->hld );
        return -1;
    }
    
    return 0;
}

int udp_rx(ncb_t *ncb) {
     int retval;
    
    do {
        retval = __udp_rx(ncb);
    } while (0 == retval);
    
    return retval;
}

int udp_txn(ncb_t *ncb, void *p) {
    int wcb;
    int errcode;
    struct tx_node *node = (struct tx_node *)p;
    socklen_t len = sizeof(struct sockaddr);
    
    while (node->offset < node->wcb) {
        wcb = sendto(ncb->sockfd, node->data + node->offset, node->wcb - node->offset, 0,
                (__CONST_SOCKADDR_ARG)&node->udp_target, len );

        /* fatal-error/connection-terminated  */
        if (0 == wcb) {
            nis_call_ecr("[nshost.udpio.udp_txn] fatal error occurred syscall sendto(2), the return value equal to zero, link:%lld", ncb->hld);
            return -1;
        }

        if (wcb < 0) {
             errcode = errno;
             
            /* the write buffer is full, active EPOLLOUT and waitting for epoll event trigger
             * at this point, we need to deal with the queue header node and restore the unprocessed node back to the queue header.
             * the way 'oneshot' focus on the write operation completion point */
            if (EAGAIN == errcode) {
                nis_call_ecr("[nshost.udpio.udp_txn] syscall sendto(2) would block cause by kernel memory overload,link:%lld", ncb->hld);
                return -EAGAIN;
            }

            /* A signal occurred before any data  was  transmitted
                continue and send again */
            if (EINTR == errcode) {
                continue;
            }
            
             /* other error, these errors should cause link close */
            nis_call_ecr("[nshost.udpio.udp_txn] fatal error occurred syscall sendto(2), error:%d, link:%lld",errcode, ncb->hld );
            return -1;
        }

        node->offset += wcb;
    }
    
    return node->wcb;
}

int udp_tx(ncb_t *ncb) {
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