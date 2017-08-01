#include <stdio.h>
#include <assert.h>
#include <errno.h>

#include "udp.h"

static
int udpi_rx(ncb_t *ncb) {
    int recvcb;
    struct sockaddr_in remote;
    socklen_t addrlen;
    udp_data_t c_data;
    nis_event_t c_event;
    int errcode;

    addrlen = sizeof (struct sockaddr_in);
    recvcb = recvfrom(ncb->sockfd, ncb->packet, UDP_BUFFER_SIZE, 0, (struct sockaddr *) &remote, &addrlen);
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
        return -1;
    }
    
    /* ECONNRESET 104 Connection reset by peer */
    if (recvcb < 0){
        if (EAGAIN == errcode){
            return EAGAIN;
        }
        
        /* 系统中断 */
        if (EINTR == errcode) {
            return 0;
        }
        return -1;
    }
    
    return 0;
}

int udp_rx(ncb_t *ncb) {
     int retval;
    
    do {
        retval = udpi_rx(ncb);
    } while (0 == retval);
    
    return retval;
}

int udp_direct_tx(ncb_t *ncb, const unsigned char *data, int *offset, int size, const struct sockaddr_in *target){
    int retval;
    int wcb;
    int errcode;
    
    if (!ncb || !data || !offset || size <= 0 || !target ) {
        return EINVAL;
    }
    
    wcb = size;
    while (*offset < wcb){
        retval = sendto(ncb->sockfd, data + *offset, wcb - *offset,
                0, (struct sockaddr *) target, sizeof ( struct sockaddr_in ));
        errcode = errno;
        if (retval < 0){
            if ( errcode == EAGAIN ) {
                return EAGAIN;
            }
            
            if (errcode == EINTR) {
                continue;
            }
            
            return -1;
        }
        
        if (0 == retval){
            return -1;
        }
        *offset += retval;
    }
    
    return 0;
}

int udp_tx(ncb_t *ncb) {
    struct packet_node_t *packet;
    int retval;

    if (!ncb) {
        return -1;
    }

    /* 如果写IO阻塞， 则直接返回重新尝试 */
    if (ncb_if_wblocked(ncb)) {
        return EAGAIN;
    }

    if (NULL == (packet = fque_get(&ncb->tx_fifo))) {
        return -1;
    }
    
     /* 仅对头节点执行操作 */
    do {
        retval = udp_direct_tx(ncb, packet->data, &packet->offset, packet->wcb, &packet->udp_target);
        
        /* 写入过程发生致命错误, 该链将被关闭 */
        if (retval < 0){
            break;
        }
        
        /* 写入缓冲区已满， 激活并等待 EPOLLOUT 才能继续执行下一片写入 */
        if ((EAGAIN == retval)) {
            fque_revert(&ncb->tx_fifo, packet);
            return EAGAIN;
        }
        
    }while( 0 );

    /* 除了需要还原队列头节点的情况， 其余任何情况都已经不再关注包节点， 可以释放 */
    PACKET_NODE_FREE(packet);
    return retval;
}