#include <stdio.h>

#include "tcp.h"

int tcp_syn(ncb_t *ncb_server) {
    int fd_client;
    struct sockaddr_in addr_income;
    socklen_t addrlen;
    ncb_t *ncb_client;
    objhld_t hld_client;
    nis_event_t c_event;
    tcp_data_t c_data;
    int errcode;
    
     addrlen = sizeof ( addr_income);
     fd_client = accept(ncb_server->fd_, (struct sockaddr *) &addr_income, &addrlen);
     errcode = errno;
     if (fd_client < 0) {
         /*�Ѿ�û�����ݿɹ�����������Ҫ����Ϊ�����߳�Ͷ�ݶ�ȡ���񣬵ȴ���һ�ε�EPOLL�߽紥��֪ͨ*/
        if ((errcode != EAGAIN) && (errcode != EINTR) && (errcode != EWOULDBLOCK)) {
            return -1;
        }
        return 0;
    }

    /* Ϊ�˷�ֹ�в������ں˻�������syn�����ܱ���ȷ���� ����ֻҪ����EAGAIN��������һ����ȡ����
     * �����������һ����ת */
    post_task(ncb_server->hld_, kTaskType_Read);
     
    hld_client = objallo(sizeof ( ncb_t), &objentry, &ncb_uninit, NULL, 0);
    if (hld_client < 0) {
        close(fd_client);
        return -1;
    }
    ncb_client = objrefr(hld_client);
    
    do {
        ncb_init(ncb_client);
        ncb_client->hld_ = hld_client;
        ncb_client->fd_ = fd_client;
        ncb_client->proto_type_ = kProtocolType_TCP;
        ncb_client->user_callback_ = ncb_server->user_callback_;
        
        /* ���غͶԶ˵ĵ�ַ�ṹ�� */
        getpeername(fd_client, (struct sockaddr *) &ncb_client->addr_remote_, &addrlen); /* �Զ˵ĵ�ַ��Ϣ */
        getsockname(fd_client, (struct sockaddr *) &ncb_client->addr_local_, &addrlen); /* ���صĵ�ַ��Ϣ */
       
         /*ETģ�ͱ��뱣�������ļ��������첽����*/
        if (io_raise_asio(ncb_client->fd_) < 0){
            break;
        }

        /* setsockopt */
        if (tcp_update_opts(ncb_client) < 0){
            break;
        }

        /*����TCP��ͨ��*/
        ncb_client->packet_ = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb_client->packet_){
            break;
        }
        ncb_client->packet_size_ = TCP_BUFFER_SIZE;

        /*���Э��ͷ*/
        ncb_client->recv_analyze_offset_ = 0;
        ncb_client->recv_buffer_ = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb_client->recv_buffer_) {
            break;
        }
        
        /* �������������ӣ� ��ע���ݰ� */
        ncb_client->on_read_ = &tcp_rx;
        ncb_client->on_write_ = &tcp_tx;
        
         /*�ص�֪ͨ�ϲ�, �����ӵ���*/
        c_event.Event = EVT_TCP_ACCEPTED;
        c_event.Ln.Tcp.Link = ncb_server->hld_;
        c_data.e.Accept.AcceptLink = hld_client;
        if (ncb_server->user_callback_) {
            ncb_server->user_callback_(&c_event, &c_data);
        }
        
        if (io_attach(fd_client, hld_client ) < 0){
            break;
        }
        
        objdefr(hld_client);
        return 0;
    } while (0);

    objdefr(hld_client);
    objclos(hld_client);
    return 0;
}

int tcp_rx(ncb_t *ncb) {
    int recvcb;
    int overplus;
    int offset;
    int cpcb;
    int errcode;

    recvcb = read(ncb->fd_, ncb->recv_buffer_, TCP_BUFFER_SIZE);
    errcode = errno;
    if (recvcb > 0) {
        cpcb = recvcb;
        overplus = recvcb;
        offset = 0;
        do {
            overplus = tcp_parse_pkt(ncb, ncb->recv_buffer_ + offset, cpcb);
            if (overplus < 0) {
                /* �ײ�Э���������ֱ�ӹرո����� */
                return -1;
            }
            offset += (cpcb - overplus);
            cpcb = overplus;
        } while (overplus > 0);

        /*�������ȷ���� tcp_receive_thread_proc �߳�����ִ�� 
          Ϊ�˷�ֹ�κ�һ�����Ӷ���, ������κ����Ӷ�����recv��Ĵ���, ���ǲ���׷������ķ��� */
        post_task(ncb->hld_, kTaskType_Read);
    }

    /*�Զ˶Ͽ�*/
    if (0 == recvcb) {
        return -1;
    }

    /* ECONNRESET 104 Connection reset by peer */
    if (recvcb < 0) {
        /*�Ѿ�û�����ݿɹ�����������Ҫ����Ϊ�����߳�Ͷ�ݶ�ȡ���񣬵ȴ���һ�ε�EPOLL�߽紥��֪ͨ*/
        if ((errcode != EAGAIN) && (errcode != EINTR) && (errcode != EWOULDBLOCK)) {
            return -1;
        }
    }
    
    return 0;
}

/*
 * ���ʹ�����
 */
int tcp_tx(ncb_t *ncb){
    int errcode;
    packet_node_t *packet;
    int retval;
    
    retval = 0;
    errcode = 0;
    if (!ncb) {
        return -1;
    }
    
    /* ���дIO������ ��ֱ�ӷ������³��� */
    if (ncb_if_wblocked(ncb)){
        return EAGAIN;
    }
    
    if (NULL == (packet = fque_get(&ncb->packet_fifo_))){
        return 0;
    }
    
    /* ����ͷ�ڵ�ִ�в��� */
    while(packet->offset_ < packet->wcb_) {
        retval = write(ncb->fd_, packet->packet_ + packet->offset_, packet->wcb_ - packet->offset_);
        errcode = errno;
        if (retval <= 0){
            break;
        }
        packet->offset_ += retval;
    }
    
    /* д�뻺���������� ����ȴ� EPOLLOUT ���ܼ���ִ����һƬд��
     * ��ʱ��Ҫ�������ͷ�ڵ㣬 ��δ������Ľڵ㻹ԭ�ض���ͷ */
    if ((EAGAIN == errcode ) && (retval < 0)){
        fque_revert(&ncb->packet_fifo_, packet);
        io_rdwr(ncb, ncb->hld_);
        return EAGAIN;
    }
    
    /* ������Ҫ��ԭ����ͷ�ڵ������� �����κ�������Ѿ����ٹ�ע���ڵ㣬 �����ͷ� */
    PACKET_NODE_FREE(packet);
    
    /* д������������ */
    if (retval > 0){
        return 0;
    }
    
    /* �Զ˶Ͽ��� �� �����������̵Ĵ��� */
    printf("failed write to socket.hld=%d, error message=%s.\n", ncb->hld_, strerror(errcode));
    objclos(ncb->hld_);
    return -1;
}