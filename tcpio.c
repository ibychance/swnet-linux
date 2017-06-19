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
         /*已经没有数据可供读出，不需要继续为工作线程投递读取任务，等待下一次的EPOLL边界触发通知*/
        if ((errcode != EAGAIN) && (errcode != EINTR) && (errcode != EWOULDBLOCK)) {
            return -1;
        }
        
        // syn 操作等效于 Rx 操作
        posix__pthread_mutex_lock(&ncb_server->rx_prot_lock);
        if (ncb_server->rx_order_count > 0){
            post_read_task(ncb_server->hld, kTaskType_RxTest);
        }else{
            ncb_server->rx_running = posix__false;
        }
        posix__pthread_mutex_unlock(&ncb_server->rx_prot_lock);
        return EAGAIN;
    }

    /* 为了防止有残留在内核缓冲区的syn请求不能被正确处理， 这里只要不是EAGAIN，均增加一个读取任务，
     * 性能损耗最多就一个空转 */
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
        /*已经没有数据可供读出，不需要继续为工作线程投递读取任务，等待下一次的EPOLL边界触发通知*/
        if ((errcode != EAGAIN) && (errcode != EINTR) && (errcode != EWOULDBLOCK)) {
            return -1;
        }
        
        /* 发生EAGAIN, 则证明当前缓冲区无数据可读，此时需要对TCP的Rx整体状态进行调整 
         * 如果有 order 计数存在， 则继续投递 attempt 任务
         * 否则， 将标志位设置为 Rx 不在运行中*/
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
        if (retval <= 0){
            break;
        }
        packet->offset += retval;
    }
    
    /* 写入缓冲区已满， 激活并等待 EPOLLOUT 才能继续执行下一片写入
     * 此时需要处理队列头节点， 将未处理完的节点还原回队列头
     * oneshot 方式强制关注写入操作完成点 */
    if ((EAGAIN == errcode ) && (retval < 0)){
        fque_revert(&ncb->tx_fifo, packet);
        return EAGAIN;
    }
    
    /* 除了需要还原队列头节点的情况， 其余任何情况都已经不再关注包节点， 可以释放 */
    PACKET_NODE_FREE(packet);
    
    /* 写入操作正常完成 */
    if (retval > 0){
        return 0;
    }
    
    /* 对端断开， 或， 其他不可容忍的错误 */
    printf("failed write to socket.hld=%d, error message=%s.\n", ncb->hld, strerror(errcode));
    return -1;
}