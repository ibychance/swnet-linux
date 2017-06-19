#include <stdio.h>

#include "tcp.h"

#include "posix_ifos.h"


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
     fd_client = accept(ncb_server->sockfd, (struct sockaddr *) &addr_income, &addrlen);
     errcode = errno;
     if (fd_client < 0) {
         /*�Ѿ�û�����ݿɹ�����������Ҫ����Ϊ�����߳�Ͷ�ݶ�ȡ���񣬵ȴ���һ�ε�EPOLL�߽紥��֪ͨ*/
        if ((errcode != EAGAIN) && (errcode != EINTR) && (errcode != EWOULDBLOCK)) {
            return -1;
        }
        
        // syn ������Ч�� Rx ����
        posix__pthread_mutex_lock(&ncb_server->rx_prot_lock);
        if (ncb_server->rx_order_count > 0){
            post_read_task(ncb_server->hld, kTaskType_RxTest);
        }else{
            ncb_server->rx_running = posix__false;
        }
        posix__pthread_mutex_unlock(&ncb_server->rx_prot_lock);
        return EAGAIN;
    }

    /* Ϊ�˷�ֹ�в������ں˻�������syn�����ܱ���ȷ���� ����ֻҪ����EAGAIN��������һ����ȡ����
     * �����������һ����ת */
    // post_read_task(ncb_server->hld, kTaskType_RxTest);
     
    hld_client = objallo(sizeof ( ncb_t), &objentry, &ncb_uninit, NULL, 0);
    if (hld_client < 0) {
        close(fd_client);
        return -1;
    }
    ncb_client = objrefr(hld_client);
    
    do {
        ncb_init(ncb_client);
        ncb_client->hld = hld_client;
        ncb_client->sockfd = fd_client;
        ncb_client->proto_type = kProtocolType_TCP;
        ncb_client->nis_callback = ncb_server->nis_callback;
        
        /* ���غͶԶ˵ĵ�ַ�ṹ�� */
        getpeername(fd_client, (struct sockaddr *) &ncb_client->remot_addr, &addrlen); /* �Զ˵ĵ�ַ��Ϣ */
        getsockname(fd_client, (struct sockaddr *) &ncb_client->local_addr, &addrlen); /* ���صĵ�ַ��Ϣ */
       
         /*ETģ�ͱ��뱣�������ļ��������첽����*/
        if (setasio(ncb_client->sockfd) < 0){
            break;
        }

        /* setsockopt */
        if (tcp_update_opts(ncb_client) < 0){
            break;
        }

        /*����TCP��ͨ��*/
        ncb_client->packet = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb_client->packet){
            break;
        }

        /*���Э��ͷ*/
        ncb_client->rx_parse_offset = 0;
        ncb_client->rx_buffer = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb_client->rx_buffer) {
            break;
        }
        
        /* �������������ӣ� ��ע���ݰ� */
        ncb_client->ncb_read = &tcp_rx;
        ncb_client->ncb_write = &tcp_tx;
        
         /*�ص�֪ͨ�ϲ�, �����ӵ���*/
        c_event.Event = EVT_TCP_ACCEPTED;
        c_event.Ln.Tcp.Link = ncb_server->hld;
        c_data.e.Accept.AcceptLink = hld_client;
        if (ncb_server->nis_callback) {
            ncb_server->nis_callback(&c_event, &c_data);
        }
        
        if (ioatth(ncb_client, kPollMask_Read ) < 0){
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
    
    recvcb = recv(ncb->sockfd, ncb->rx_buffer, TCP_BUFFER_SIZE, 0);
    errcode = errno;
    if (recvcb > 0) {
        cpcb = recvcb;
        overplus = recvcb;
        offset = 0;
        do {
            overplus = tcp_parse_pkt(ncb, ncb->rx_buffer + offset, cpcb);
            if (overplus < 0) {
                /* �ײ�Э���������ֱ�ӹرո����� */
                return -1;
            }
            offset += (cpcb - overplus);
            cpcb = overplus;
        } while (overplus > 0);
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
        
        /* ����EAGAIN, ��֤����ǰ�����������ݿɶ�����ʱ��Ҫ��TCP��Rx����״̬���е��� 
         * ����� order �������ڣ� �����Ͷ�� attempt ����
         * ���� ����־λ����Ϊ Rx ����������*/
        posix__pthread_mutex_lock(&ncb->rx_prot_lock);
        if (ncb->rx_order_count > 0){
            post_read_task(ncb->hld, kTaskType_RxTest);
        }else{
            ncb->rx_running = posix__false;
        }
        posix__pthread_mutex_unlock(&ncb->rx_prot_lock);
        return EAGAIN;
    }
    
    return 0;
}

/*
 * ���ʹ�����
 */
int tcp_tx(ncb_t *ncb){
    int errcode;
    struct packet_node_t *packet;
    int retval;
    
    retval = 0;
    errcode = 0;
    if (!ncb) {
        return -1;
    }
    
    if (NULL == (packet = fque_get(&ncb->tx_fifo))){
        return 1;
    }
    
    /* ����ͷ�ڵ�ִ�в��� */
    while(packet->offset < packet->wcb) {
        retval = write(ncb->sockfd, packet->data + packet->offset, packet->wcb - packet->offset);
        errcode = errno;
        if (retval <= 0){
            break;
        }
        packet->offset += retval;
    }
    
    /* д�뻺���������� ����ȴ� EPOLLOUT ���ܼ���ִ����һƬд��
     * ��ʱ��Ҫ�������ͷ�ڵ㣬 ��δ������Ľڵ㻹ԭ�ض���ͷ
     * oneshot ��ʽǿ�ƹ�עд�������ɵ� */
    if ((EAGAIN == errcode ) && (retval < 0)){
        fque_revert(&ncb->tx_fifo, packet);
        return EAGAIN;
    }
    
    /* ������Ҫ��ԭ����ͷ�ڵ������� �����κ�������Ѿ����ٹ�ע���ڵ㣬 �����ͷ� */
    PACKET_NODE_FREE(packet);
    
    /* д������������ */
    if (retval > 0){
        return 0;
    }
    
    /* �Զ˶Ͽ��� �� �����������̵Ĵ��� */
    printf("failed write to socket.hld=%d, error message=%s.\n", ncb->hld, strerror(errcode));
    return -1;
}