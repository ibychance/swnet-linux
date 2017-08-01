#include <stdio.h>

#include "tcp.h"

#include "posix_ifos.h"

static
int tcpi_syn(ncb_t *ncb_server) {
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
         
         /* ϵͳ�����жϣ� ��������ִ����һ�ζ�ȡ���� */
         if (errcode == EINTR) {
             return 0;
         }
         
         /*�Ѿ�û�����ݿɹ�����������Ҫ����Ϊ�����߳�Ͷ�ݶ�ȡ���񣬵ȴ���һ�ε�EPOLL�߽紥��֪ͨ*/
        if ((errcode == EAGAIN) || (errcode == EWOULDBLOCK)) {
            return EAGAIN;
        }
         
        return -1;
    }
     
     /* �Ѿ��õ��˶Զ����ӣ� �����ڴ���ͻ����ӵĳ�ʼ���Ȳ���ʱ������ʧ�� */
    hld_client = objallo(sizeof ( ncb_t), &objentry, &ncb_uninit, NULL, 0);
    if (hld_client < 0) {
        close(fd_client);
        return 0;
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

int tcp_syn(ncb_t *ncb_server) {
    int retval;
    
    tcp_save_info(ncb_server);
    
    do {
        retval = tcpi_syn(ncb_server);
    } while (0 == retval);
    return retval;
}

static
int tcpi_rx(ncb_t *ncb){
    int recvcb;
    int overplus;
    int offset;
    int cpcb;
    int errcode;
    
    tcp_save_info(ncb);
    
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
        
        /* �κ�ϵͳ�жϵ��µĶ����ݷ��أ��һ�û���κ��������ü�д��Ӧ�ò㻺����
         * ��ʱӦ��������ִ��һ�ζ����� */
        if (errcode == EINTR) {
            return 0;
        }
        
        /*�Ѿ�û�����ݿɹ�����������Ҫ����Ϊ�����߳�Ͷ�ݶ�ȡ���񣬵ȴ���һ�ε�EPOLL�߽紥��֪ͨ*/
        if ((errcode == EAGAIN) || (errcode == EWOULDBLOCK)) {
            return EAGAIN;
        }
        
        return -1;
    }
    return 0;
}

int tcp_rx(ncb_t *ncb) {
    int retval;
    
    tcp_save_info(ncb);
    
    /* �����ջ���������Ϊֹ */
    do {
        retval = tcpi_rx(ncb);
    } while (0 == retval);
    return retval;
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
        
        /* �Զ˶Ͽ��� �� �����������̵Ĵ��� */
        if (0 == retval){
            printf("failed write to socket.hld=%d, error message=%s.\n", ncb->hld, strerror(errcode));
            retval = -1;
            break;
        }
        
        if (retval < 0){
            
            /* д�뻺���������� ����ȴ� EPOLLOUT ���ܼ���ִ����һƬд��
            * ��ʱ��Ҫ�������ͷ�ڵ㣬 ��δ������Ľڵ㻹ԭ�ض���ͷ
            * oneshot ��ʽǿ�ƹ�עд�������ɵ� */
            if (EAGAIN == errcode ) {
                fque_revert(&ncb->tx_fifo, packet);
                return EAGAIN;
            }
            
            /* �ж��źŵ��µ�д������ֹ������û���κ�һ���ֽ����д�룬���Ծ͵ػָ� */
            if (EINTR == errcode){
                continue;
            }
            
            printf("failed write to socket.hld=%d, error message=%s.\n", ncb->hld, strerror(errcode));
            return -1;
        }
        
        packet->offset += retval;
    }
    
    PACKET_NODE_FREE(packet);
    return 0;
}