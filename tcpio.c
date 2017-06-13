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
         /*已经没有数据可供读出，不需要继续为工作线程投递读取任务，等待下一次的EPOLL边界触发通知*/
        if ((errcode != EAGAIN) && (errcode != EINTR) && (errcode != EWOULDBLOCK)) {
            return -1;
        }
        return 0;
    }

    /* 为了防止有残留在内核缓冲区的syn请求不能被正确处理， 这里只要不是EAGAIN，均增加一个读取任务，
     * 性能损耗最多就一个空转 */
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
        
        /* 本地和对端的地址结构体 */
        getpeername(fd_client, (struct sockaddr *) &ncb_client->addr_remote_, &addrlen); /* 对端的地址信息 */
        getsockname(fd_client, (struct sockaddr *) &ncb_client->addr_local_, &addrlen); /* 本地的地址信息 */
       
         /*ET模型必须保持所有文件描述符异步进行*/
        if (io_raise_asio(ncb_client->fd_) < 0){
            break;
        }

        /* setsockopt */
        if (tcp_update_opts(ncb_client) < 0){
            break;
        }

        /*分配TCP普通包*/
        ncb_client->packet_ = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb_client->packet_){
            break;
        }
        ncb_client->packet_size_ = TCP_BUFFER_SIZE;

        /*清空协议头*/
        ncb_client->recv_analyze_offset_ = 0;
        ncb_client->recv_buffer_ = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb_client->recv_buffer_) {
            break;
        }
        
        /* 接收上来的链接， 关注数据包 */
        ncb_client->on_read_ = &tcp_rx;
        ncb_client->on_write_ = &tcp_tx;
        
         /*回调通知上层, 有链接到来*/
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
                /* 底层协议解析出错，直接关闭该链接 */
                return -1;
            }
            offset += (cpcb - overplus);
            cpcb = overplus;
        } while (overplus > 0);

        /*这个过程确定在 tcp_receive_thread_proc 线程锁内执行 
          为了防止任何一个链接饿死, 这里对任何链接都不作recv完的处理, 而是采用追加任务的方法 */
        post_task(ncb->hld_, kTaskType_Read);
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
    }
    
    return 0;
}

/*
 * 发送处理器
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
    
    /* 如果写IO阻塞， 则直接返回重新尝试 */
    if (ncb_if_wblocked(ncb)){
        return EAGAIN;
    }
    
    if (NULL == (packet = fque_get(&ncb->packet_fifo_))){
        return 0;
    }
    
    /* 仅对头节点执行操作 */
    while(packet->offset_ < packet->wcb_) {
        retval = write(ncb->fd_, packet->packet_ + packet->offset_, packet->wcb_ - packet->offset_);
        errcode = errno;
        if (retval <= 0){
            break;
        }
        packet->offset_ += retval;
    }
    
    /* 写入缓冲区已满， 激活并等待 EPOLLOUT 才能继续执行下一片写入
     * 此时需要处理队列头节点， 将未处理完的节点还原回队列头 */
    if ((EAGAIN == errcode ) && (retval < 0)){
        fque_revert(&ncb->packet_fifo_, packet);
        io_rdwr(ncb, ncb->hld_);
        return EAGAIN;
    }
    
    /* 除了需要还原队列头节点的情况， 其余任何情况都已经不再关注包节点， 可以释放 */
    PACKET_NODE_FREE(packet);
    
    /* 写入操作正常完成 */
    if (retval > 0){
        return 0;
    }
    
    /* 对端断开， 或， 其他不可容忍的错误 */
    printf("failed write to socket.hld=%d, error message=%s.\n", ncb->hld_, strerror(errcode));
    objclos(ncb->hld_);
    return -1;
}