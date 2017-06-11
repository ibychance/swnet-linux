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
    if ((recvcb = recvfrom(ncb->fd_, ncb->packet_, ncb->packet_size_, 0, (struct sockaddr *) &remote, &addrlen)) > 0) {
        c_event.Ln.Udp.Link = ncb->hld_;
        c_event.Event = EVT_RECEIVEDATA;
        c_data.e.Packet.Data = ncb->packet_;
        c_data.e.Packet.Size = recvcb;
        inet_ntop(AF_INET, &remote.sin_addr, c_data.e.Packet.RemoteAddress, sizeof (c_data.e.Packet.RemoteAddress));
        c_data.e.Packet.RemotePort = ntohs(remote.sin_port);
        if (ncb->user_callback_) {
            ncb->user_callback_(&c_event, &c_data);
        }

        /*�������ȷ���� tcp_receive_thread_proc �߳�����ִ�� 
           Ϊ�˷�ֹ�κ�һ�����Ӷ���, ������κ����Ӷ�����recv��Ĵ���, ���ǲ���׷������ķ��� */
        post_task(ncb->hld_, kTaskType_Read);
    }

    /* ECONNRESET 104 Connection reset by peer */
    if ((recvcb == 0) || ((recvcb < 0) && (EAGAIN != errno))) {
        return -1;
    }

    return 0;
}

int udp_tx(ncb_t *ncb) {
    int errcode;
    packet_node_t *packet;
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

    if (NULL == (packet = fque_get(&ncb->packet_fifo_))) {
        return -1;
    }

    /* ����ͷ�ڵ�ִ�в��� */
    while (packet->offset_ < packet->wcb_) {
        assert(packet->wcb_ > 0);
        retval = sendto(ncb->fd_, packet->packet_ + packet->offset_, packet->wcb_ - packet->offset_,
                0, (struct sockaddr *) &packet->target_, sizeof ( packet->target_));
        errcode = errno;
        if (retval <= 0) {
            break;
        }
        packet->offset_ += retval;
    }

    /* д�뻺���������� ����ȴ� EPOLLOUT ���ܼ���ִ����һƬд�� */
    if ((EAGAIN == errcode) && (retval < 0)) {
        fque_revert(&ncb->packet_fifo_, packet);
        io_rdwr(ncb, ncb->hld_);
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