#include <stdio.h>
#include <assert.h>
#include <errno.h>

#include "udp.h"
#include "mxx.h"

static
int __udp_rx(ncb_t *ncb) {
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
        
        /* system interrupted */
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
        retval = __udp_rx(ncb);
    } while (0 == retval);
    
    return retval;
}

static
int __udp_tx_single_packet(int sockfd, struct tx_node *packet){
    int wcb;
    int errcode;
    socklen_t len = sizeof(struct sockaddr);
    
    /* 仅对头节点执行操作 */
    while (packet->offset < packet->wcb) {
        wcb = sendto(sockfd, packet->data + packet->offset, packet->wcb - packet->offset, 0,
                (__CONST_SOCKADDR_ARG)&packet->udp_target, len );

        /* 对端断开， 或， 其他不可容忍的错误 */
        if (0 == wcb) {
            return -1;
        }

        if (wcb < 0) {
             errcode = errno;
             
            /* 写入缓冲区已满， 激活并等待 EPOLLOUT 才能继续执行下一片写入
             * 此时需要处理队列头节点， 将未处理完的节点还原回队列头
             * oneshot 方式强制关注写入操作完成点 */
            if (EAGAIN == errcode) {
                return EAGAIN;
            }

            /* 中断信号导致的写操作中止，而且没有任何一个字节完成写入，可以就地恢复 */
            if (EINTR == errcode) {
                continue;
            }
            
             /* 发生其他无法容忍且无法处理的错误, 这个错误返回会导致断开链接 */
            return make_error_result(errcode);
        }

        packet->offset += wcb;
    }
    
    return 0;
}

int udp_tx(ncb_t *ncb) {
    struct tx_node *packet;
    int retval;
    
    if (!ncb) {
        return -1;
    }
    
    /* 若无特殊情况， 需要把所有发送缓冲包全部写入内核 */
    while (NULL != (packet = fque_get(&ncb->tx_fifo))) {
        retval = __udp_tx_single_packet(ncb->sockfd, packet);
        if (retval < 0) {
            return retval;
        }  else {
            if (EAGAIN == retval) {
                fque_revert(&ncb->tx_fifo, packet);
                return EAGAIN;
            }
        }

        PACKET_NODE_FREE(packet);
    }
    
    return 0;
}