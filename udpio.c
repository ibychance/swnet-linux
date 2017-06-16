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

        /*�������ȷ���� tcp_receive_thread_proc �߳�����ִ�� 
           Ϊ�˷�ֹ�κ�һ�����Ӷ���, ������κ����Ӷ�����recv��Ĵ���, ���ǲ���׷������ķ��� */
        post_task(ncb->hld, kTaskType_RxOrder);
    }

    /* ECONNRESET 104 Connection reset by peer */
    if ((recvcb == 0) || ((recvcb < 0) && (EAGAIN != errno))) {
        return -1;
    }

    return 0;
}

int udp_tx(ncb_t *ncb) {
    int errcode;
    struct packet_node_t *packet;
    int retval;

    retval = 0;
    errcode = 0;
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
    while (packet->offset < packet->wcb) {
        assert(packet->wcb > 0);
        retval = sendto(ncb->sockfd, packet->data + packet->offset, packet->wcb - packet->offset,
                0, (struct sockaddr *) &packet->udp_target, sizeof ( packet->udp_target));
        errcode = errno;
        if (retval <= 0) {
            break;
        }
        packet->offset += retval;
    }

    /* д�뻺���������� ����ȴ� EPOLLOUT ���ܼ���ִ����һƬд�� */
    if ((EAGAIN == errcode) && (retval < 0)) {
        fque_revert(&ncb->tx_fifo, packet);
        iordwr(ncb, ncb->hld);
        return EAGAIN;
    }

    /* ������Ҫ��ԭ����ͷ�ڵ������� �����κ�������Ѿ����ٹ�ע���ڵ㣬 �����ͷ� */
    PACKET_NODE_FREE(packet);

    /* д������������ */
    if (retval > 0) {
        return 0;
    }

    /* �������̵Ĵ��� */
    ncb_report_debug_information(ncb, "failed write to socket, error message=%s.\n", strerror(errcode));
    return -1;
}