#include <stdio.h>

#include "tcp.h"

static int tcp_user_syn(ncb_t *ncb_server, struct user_event_node_t *userio_node) {
    int fd_client;
    socklen_t addrlen;
    ncb_t *ncb_client;
    objhld_t hld_client;
    nis_event_t c_event;
    tcp_data_t c_data;

    if (!ncb_server->user_callback_) {
        return -1;
    }
    
    fd_client = userio_node->fd;

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

        /*回调通知上层, 有链接到来, 这一操作必须在加入epoll监控前，否则将导致accept和recv事件乱序*/
        c_event.Event = EVT_TCP_ACCEPTED;
        c_event.Ln.Tcp.Link = ncb_server->hld_;
        c_data.e.Accept.AcceptLink = hld_client;
        ncb_server->user_callback_(&c_event, &c_data);

        /*ET模型必须保持所有文件描述符异步进行*/
        if (io_raise_asio(ncb_client->fd_) < 0) {
            break;
        }

        /* setsockopt */
        if (tcp_update_opts(ncb_client) < 0) {
            break;
        }

        /*分配TCP普通包*/
        ncb_client->packet_ = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb_client->packet_) {
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
        ncb_client->on_userio_ = &tcp_userio;

        if (io_attach(fd_client, hld_client) < 0) {
            break;
        }

        objdefr(hld_client);
        return 0;
    } while (0);

    objdefr(hld_client);
    objclos(hld_client);
    return -1;
}

static int tcp_user_rx(ncb_t *ncb, struct user_event_node_t *userio_node){
    int overplus;
    int offset;
    int cpcb;
    int recvcb;
    struct user_rx_node_t *rx_node;

    rx_node = (struct user_rx_node_t *)userio_node->event_body;
    
    recvcb = rx_node->offset_;
    cpcb = recvcb;
    overplus = recvcb;
    offset = 0;
    do {
        overplus = tcp_parse_pkt(ncb, rx_node->buffer_ + offset, cpcb);
        if (overplus < 0) {
            /* 底层协议解析出错，直接关闭该链接 */
            return -1;
        }
        offset += (cpcb - overplus);
        cpcb = overplus;
    } while (overplus > 0);
    
    return 0;
}

int tcp_userio(ncb_t *ncb) {
    struct user_event_node_t *userio_node;
    int retval;
    
    retval = 0;

    posix__pthread_mutex_lock(&ncb->userio_lock_);
    userio_node = list_first_entry_or_null(&ncb->userio_list_, struct user_event_node_t, link);
    if (userio_node) {
        list_del(&userio_node->link);
    } else {
        ncb->if_userio_running_ = posix__false;
    }
    posix__pthread_mutex_unlock(&ncb->userio_lock_);
    
    if (userio_node){
        switch(userio_node->event){
            case kUserEvent_Rx:
                retval = tcp_user_rx(ncb, userio_node);
                break;
            case kUserEvent_Syn:
                retval = tcp_user_syn(ncb, userio_node);
                break;
            default:
                retval = -1;
                break;
        }
        
         free(userio_node);
         
        /* 只要当前链表非空， 就无条件再追加一个解析任务， 顶多空转一圈 */
         posix__pthread_mutex_lock(&ncb->userio_lock_);
         if (!list_empty(&ncb->userio_list_)) {
             post_task(ncb->hld_, kTaskType_User);
         }else {
            ncb->if_userio_running_ = posix__false;
        }
         posix__pthread_mutex_unlock(&ncb->userio_lock_);
        
    }

    return retval;
}

int tcp_syn(ncb_t *ncb_server) {
    int fd_client;
    struct sockaddr_in addr_income;
    socklen_t addrlen;
    int errcode;
    struct user_event_node_t *syn_node;

    if (!ncb_server->user_callback_) {
        return -1;
    }

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
    
    if (NULL == (syn_node = (struct user_event_node_t *) malloc(sizeof (struct user_event_node_t)))) {
        close(fd_client);
        
        /* 不能为远端链接分配空间， 不需要返回-1
         * 因为远端链接不能建立的错误， 不影响监听链接继续工作 */
        return 0;
    }
    syn_node->event = kUserEvent_Syn;
    syn_node->fd = fd_client;
    ncb_post_user_task(ncb_server, syn_node);
    
    /* 再次投递一个都请求，去获取ET模式下可能没有完全获取的链接 */
    post_task(ncb_server->hld_, kTaskType_Read);
    return 0;
}

int tcp_rx(ncb_t *ncb) {
    int recvcb;
    int errcode;
    struct user_event_node_t *rx_node;
    struct user_rx_node_t *rx_part;
    
    if (NULL == (rx_node = (struct user_event_node_t *) malloc(sizeof (struct user_event_node_t) + sizeof(struct user_rx_node_t)))) {
        return -1;
    }
    rx_node->event = kUserEvent_Rx;
    rx_node->fd = ncb->fd_;
    rx_part = (struct user_rx_node_t *)rx_node->event_body;
    
    recvcb = read(ncb->fd_, rx_part->buffer_, sizeof (rx_part->buffer_));
    errcode = errno;
    if (recvcb > 0) {
        rx_part->offset_ = recvcb;
        ncb_post_user_task(ncb, rx_node);

        /*这个过程确定在 tcp_receive_thread_proc 线程锁内执行 
          为了防止任何一个链接饿死, 这里对任何链接都不作recv完的处理, 而是采用追加任务的方法 */
        post_task(ncb->hld_, kTaskType_Read);
        return 0;
    }

    free(rx_node);

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
int tcp_tx(ncb_t *ncb) {
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

    if (NULL == (packet = fque_get(&ncb->packet_fifo_))) {
        return 0;
    }

    /* 仅对头节点执行操作 */
    while (packet->offset_ < packet->wcb_) {
        retval = write(ncb->fd_, packet->packet_ + packet->offset_, packet->wcb_ - packet->offset_);
        errcode = errno;
        if (retval <= 0) {
            break;
        }
        packet->offset_ += retval;
    }

    /* 写入缓冲区已满， 激活并等待 EPOLLOUT 才能继续执行下一片写入
     * 此时需要处理队列头节点， 将未处理完的节点还原回队列头 */
    if ((EAGAIN == errcode) && (retval < 0)) {
        fque_revert(&ncb->packet_fifo_, packet);
        io_rdwr(ncb, ncb->hld_);
        return EAGAIN;
    }

    /* 除了需要还原队列头节点的情况， 其余任何情况都已经不再关注包节点， 可以释放 */
    PACKET_NODE_FREE(packet);

    /* 写入操作正常完成 */
    if (retval > 0) {
        return 0;
    }

    /* 对端断开， 或， 其他不可容忍的错误 */
    printf("failed write to socket.hld=%d, error message=%s.\n", ncb->hld_, strerror(errcode));
    objclos(ncb->hld_);
    return -1;
}