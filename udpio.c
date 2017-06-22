#include <stdio.h>
#include <assert.h>

#include "udp.h"

int udp_rx(ncb_t *ncb) {
    int recvcb;
    struct sockaddr_in remote;
    socklen_t addrlen;
    udp_data_t c_data;
    nis_event_t c_event;

    addrlen = sizeof (struct sockaddr_in);
    if ((recvcb = recvfrom(ncb->sockfd, ncb->packet, UDP_BUFFER_SIZE, 0, (struct sockaddr *) &remote, &addrlen)) > 0) {
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

    /* ECONNRESET 104 Connection reset by peer */
    if ((recvcb == 0) || ((recvcb < 0) && (EAGAIN != errno))) {
        return -1;
    }

    return 0;
}

int udp_direct_tx(ncb_t *ncb, const unsigned char *data, int *offset, int size, const struct sockaddr_in *target){
    int retval;
    int wcb;
    
    if (!ncb || !data || !offset || size <= 0 || !target ) {
        return EINVAL;
    }
    
    wcb = size;
    while (*offset < wcb){
        retval = sendto(ncb->sockfd, data + *offset, wcb - *offset,
                0, (struct sockaddr *) target, sizeof ( struct sockaddr_in ));
        if (retval < 0){
            return errno;
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

    /* ���дIO������ ��ֱ�ӷ������³��� */
    if (ncb_if_wblocked(ncb)) {
        return EAGAIN;
    }

    if (NULL == (packet = fque_get(&ncb->tx_fifo))) {
        return -1;
    }
    
     /* ����ͷ�ڵ�ִ�в��� */
    do {
        retval = udp_direct_tx(ncb, packet->data, &packet->offset, packet->wcb, &packet->udp_target);
        if (retval <= 0){
            break;
        }
        
        /* д�뻺���������� ����ȴ� EPOLLOUT ���ܼ���ִ����һƬд�� */
        if ((EAGAIN == retval)) {
            fque_revert(&ncb->tx_fifo, packet);
            ncb_mark_wblocked(ncb);
            iomod(ncb, kPollMask_Read | kPollMask_Write);
            return EAGAIN;
        }
        
    }while( 0 );

    /* ������Ҫ��ԭ����ͷ�ڵ������� �����κ�������Ѿ����ٹ�ע���ڵ㣬 �����ͷ� */
    PACKET_NODE_FREE(packet);
    return retval;
}