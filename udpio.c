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
        
        /* ϵͳ�ж� */
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

static
int udp_tx_single_packet(int sockfd, struct tx_node *packet){
    int wcb;
    int errcode;
    socklen_t len = sizeof(struct sockaddr);
    
    /* ����ͷ�ڵ�ִ�в��� */
    while (packet->offset < packet->wcb) {
        wcb = sendto(sockfd, packet->data + packet->offset, packet->wcb - packet->offset, 0,
                (__CONST_SOCKADDR_ARG)&packet->udp_target, len );

        /* �Զ˶Ͽ��� �� �����������̵Ĵ��� */
        if (0 == wcb) {
            return -1;
        }

        if (wcb < 0) {
             errcode = errno;
             
            /* д�뻺���������� ����ȴ� EPOLLOUT ���ܼ���ִ����һƬд��
             * ��ʱ��Ҫ�������ͷ�ڵ㣬 ��δ������Ľڵ㻹ԭ�ض���ͷ
             * oneshot ��ʽǿ�ƹ�עд�������ɵ� */
            if (EAGAIN == errcode) {
                return EAGAIN;
            }

            /* �ж��źŵ��µ�д������ֹ������û���κ�һ���ֽ����д�룬���Ծ͵ػָ� */
            if (EINTR == errcode) {
                continue;
            }
            
             /* ���������޷��������޷�����Ĵ���, ������󷵻ػᵼ�¶Ͽ����� */
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
    
    /* ������������� ��Ҫ�����з��ͻ����ȫ��д���ں� */
    while (NULL != (packet = fque_get(&ncb->tx_fifo))) {
        retval = udp_tx_single_packet(ncb->sockfd, packet);
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