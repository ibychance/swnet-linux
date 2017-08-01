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
         
         /* 系统调用中断， 可以立即执行再一次读取操作 */
         if (errcode == EINTR) {
             return 0;
         }
         
         /*已经没有数据可供读出，不需要继续为工作线程投递读取任务，等待下一次的EPOLL边界触发通知*/
        if ((errcode == EAGAIN) || (errcode == EWOULDBLOCK)) {
            return EAGAIN;
        }
         
        return -1;
    }
     
     /* 已经得到了对端链接， 无需在处理客户链接的初始化等操作时，返回失败 */
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
        
        /* 本地和对端的地址结构体 */
        getpeername(fd_client, (struct sockaddr *) &ncb_client->remot_addr, &addrlen); /* 对端的地址信息 */
        getsockname(fd_client, (struct sockaddr *) &ncb_client->local_addr, &addrlen); /* 本地的地址信息 */
       
         /*ET模型必须保持所有文件描述符异步进行*/
        if (setasio(ncb_client->sockfd) < 0){
            break;
        }

        /* setsockopt */
        if (tcp_update_opts(ncb_client) < 0){
            break;
        }

        /*分配TCP普通包*/
        ncb_client->packet = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb_client->packet){
            break;
        }

        /*清空协议头*/
        ncb_client->rx_parse_offset = 0;
        ncb_client->rx_buffer = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb_client->rx_buffer) {
            break;
        }
        
        /* 接收上来的链接， 关注数据包 */
        ncb_client->ncb_read = &tcp_rx;
        ncb_client->ncb_write = &tcp_tx;
        
         /*回调通知上层, 有链接到来*/
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
                /* 底层协议解析出错，直接关闭该链接 */
                return -1;
            }
            offset += (cpcb - overplus);
            cpcb = overplus;
        } while (overplus > 0);
    }

    /*对端断开*/
    if (0 == recvcb) {
        return -1;
    }
    
    /* ECONNRESET 104 Connection reset by peer */
    if (recvcb < 0) {
        
        /* 任何系统中断导致的读数据返回，且还没有任何数据来得及写入应用层缓冲区
         * 此时应该立即再执行一次读操作 */
        if (errcode == EINTR) {
            return 0;
        }
        
        /*已经没有数据可供读出，不需要继续为工作线程投递读取任务，等待下一次的EPOLL边界触发通知*/
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
    
    /* 将接收缓冲区读空为止 */
    do {
        retval = tcpi_rx(ncb);
    } while (0 == retval);
    return retval;
}

/*
 * 发送处理器
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
    
    /* 仅对头节点执行操作 */
    while(packet->offset < packet->wcb) {
        retval = write(ncb->sockfd, packet->data + packet->offset, packet->wcb - packet->offset);
        errcode = errno;
        
        /* 对端断开， 或， 其他不可容忍的错误 */
        if (0 == retval){
            printf("failed write to socket.hld=%d, error message=%s.\n", ncb->hld, strerror(errcode));
            retval = -1;
            break;
        }
        
        if (retval < 0){
            
            /* 写入缓冲区已满， 激活并等待 EPOLLOUT 才能继续执行下一片写入
            * 此时需要处理队列头节点， 将未处理完的节点还原回队列头
            * oneshot 方式强制关注写入操作完成点 */
            if (EAGAIN == errcode ) {
                fque_revert(&ncb->tx_fifo, packet);
                return EAGAIN;
            }
            
            /* 中断信号导致的写操作中止，而且没有任何一个字节完成写入，可以就地恢复 */
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