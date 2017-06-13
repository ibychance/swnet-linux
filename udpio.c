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
        c_event.Ln.Udp.Link = ncb->hld_;
        c_event.Event = EVT_RECEIVEDATA;
        c_data.e.Packet.Data = ncb->packet;
        c_data.e.Packet.Size = recvcb;
        inet_ntop(AF_INET, &remote.sin_addr, c_data.e.Packet.RemoteAddress, sizeof (c_data.e.Packet.RemoteAddress));
        c_data.e.Packet.RemotePort = ntohs(remote.sin_port);
        if (ncb->nis_callback) {
            ncb->nis_callback(&c_event, &c_data);
        }

        /*这个过程确定在 tcp_receive_thread_proc 线程锁内执行 
           为了防止任何一个链接饿死, 这里对任何链接都不作recv完的处理, 而是采用追加任务的方法 */
        post_task(ncb->hld_, kTaskType_RxOrder);
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

    /* 如果写IO阻塞， 则直接返回重新尝试 */
    if (ncb_if_wblocked(ncb)) {
        return EAGAIN;
    }

    if (NULL == (packet = fque_get(&ncb->tx_fifo))) {
        return -1;
    }

    /* 仅对头节点执行操作 */
    while (packet->offset_ < packet->wcb_) {
        assert(packet->wcb_ > 0);
        retval = sendto(ncb->sockfd, packet->packet_ + packet->offset_, packet->wcb_ - packet->offset_,
                0, (struct sockaddr *) &packet->target_, sizeof ( packet->target_));
        errcode = errno;
        if (retval <= 0) {
            break;
        }
        packet->offset_ += retval;
    }

    /* 写入缓冲区已满， 激活并等待 EPOLLOUT 才能继续执行下一片写入 */
    if ((EAGAIN == errcode) && (retval < 0)) {
        fque_revert(&ncb->tx_fifo, packet);
        iordwr(ncb, ncb->hld_);
        return EAGAIN;
    }

    /* 除了需要还原队列头节点的情况， 其余任何情况都已经不再关注包节点， 可以释放 */
    PACKET_NODE_FREE(packet);

    /* 写入操作正常完成 */
    if (retval > 0) {
        return 0;
    }

    /* 不可容忍的错误 */
    ncb_report_debug_information(ncb, "failed write to socket, error message=%s.\n", strerror(errcode));
    return -1;
}