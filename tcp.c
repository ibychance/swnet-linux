#include <stdio.h>
#include <time.h>
#include <sys/select.h>
#include <sys/time.h>

#include <linux/tcp.h>

#include "tcp.h"

int tcp_update_opts(ncb_t *ncb) {
    int optval;
    struct linger lgr;
    int fd;

    if (!ncb) {
        return -1;
    }
    fd = ncb->fd_;

    optval = TCP_BUFFER_SIZE;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *) &optval, sizeof ( optval)) < 0) {
        ncb_report_debug_information(ncb, "failed to set SO_RCVBUF, errno=%d\n", errno);
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char *) &optval, sizeof ( optval)) < 0) {
        ncb_report_debug_information(ncb, "failed to set SO_SNDBUF, errno=%d\n", errno);
        return -1;
    }

    lgr.l_onoff = 0;
    lgr.l_linger = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, (char *) &lgr, sizeof ( struct linger)) < 0) {
        ncb_report_debug_information(ncb, "failed to set SO_LINGER, errno=%d\n", errno);
        return -1;
    }

    /* 为保证小包效率， 禁用 Nginx 算法 */
    optval = NS_TCP_NODELAY_SET;
    if (setsockopt(ncb->fd_, IPPROTO_TCP, TCP_NODELAY, (char *) &optval, sizeof ( optval)) < 0) {
        ncb_report_debug_information(ncb, "failed to set TCP_NODELAY, errno=%d\n", errno);
        return -1;
    }

    return 0;
}

/* tcp impls */
int tcp_init() {
    if (io_init() >= 0) {
        return wtpinit(0, 0);
    }
    return -1;
}

void tcp_uninit() {
    wtpuninit();
    io_uninit();
}

HTCPLINK tcp_create(tcp_io_callback_t user_callback, const char* l_ipstr, uint16_t l_port) {
    int fd;
    struct sockaddr_in addrlocal;
    int retval;
    int optval;
    ncb_t *ncb;
    objhld_t hld = -1;

    if (!user_callback) return INVALID_HTCPLINK;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("[TCP] failed create services socket.errno=%d.\n", errno);
        return -1;
    }

    /* 允许端口复用 */
    optval = 1;
    retval = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof ( optval));

    /* 绑定地址，建立对象 */
    addrlocal.sin_addr.s_addr = l_ipstr ? inet_addr(l_ipstr) : INADDR_ANY;
    addrlocal.sin_family = AF_INET;
    addrlocal.sin_port = htons(l_port);
    retval = bind(fd, (struct sockaddr *) &addrlocal, sizeof ( struct sockaddr));
    if (retval < 0) {
        printf("[TCP] failed bind address.errno=%d.\n", errno);
        close(fd);
        return -1;
    }

    hld = objallo(sizeof ( ncb_t), &objentry, &ncb_uninit, NULL, 0);
    if (hld < 0) {
        close(fd);
        return -1;
    }
    ncb = objrefr(hld);
    if (!ncb) {
        close(fd);
        return -1;
    }

    do {
        ncb_init(ncb);
        ncb->hld_ = hld;
        ncb->fd_ = fd;
        ncb->proto_type_ = kProtocolType_TCP;
        ncb->user_callback_ = user_callback;
        memcpy(&ncb->addr_local_, &addrlocal, sizeof (addrlocal));

        /*ET模型必须保持所有文件描述符异步进行*/
        if (io_raise_asio(ncb->fd_) < 0) {
            break;
        }

        /* setsockopt */
        if (tcp_update_opts(ncb) < 0) {
            break;
        }

        /*分配TCP普通包*/
        ncb->packet_size_ = TCP_BUFFER_SIZE;
        ncb->packet_ = (char *) malloc(ncb->packet_size_);
        if (!ncb->packet_) {
            break;
        }
        memset(ncb->packet_, 0, ncb->packet_size_);

        /*清空协议头*/
        ncb->recv_analyze_offset_ = 0;
        ncb->recv_buffer_ = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb->recv_buffer_) {
            break;
        }

        objdefr(hld);
        return hld;
    } while (0);

    objdefr(hld);
    objclos(hld);
    return -1;
}

int tcp_settst(HTCPLINK lnk, const tst_t *tst) {
    ncb_t *ncb;

    if (lnk < 0 || !tst) return -1;

    ncb = (ncb_t *) objrefr((objhld_t) lnk);
    if (!ncb) return -1;

    ncb->tst_.cb_ = tst->cb_;
    ncb->tst_.builder_ = tst->builder_;
    ncb->tst_.parser_ = tst->parser_;
    objdefr((objhld_t) lnk);
    return 0;
}

int tcp_gettst(HTCPLINK lnk, tst_t *tst) {
    ncb_t *ncb;

    if (lnk < 0 || !tst) return -1;

    ncb = (ncb_t *) objrefr((objhld_t) lnk);
    if (!ncb) return -1;

    tst->cb_ = ncb->tst_.cb_;
    tst->builder_ = ncb->tst_.builder_;
    tst->parser_ = ncb->tst_.parser_;
    return 0;
}

void tcp_destroy(HTCPLINK lnk) {
    objclos(lnk);
}

/* <tcp_check_connection_bypoll>
 * static int tcp_check_connection_bypoll(int sockfd) {
 *  struct pollfd fd;
 *   fd.fd = sockfd;
 *   fd.events = POLLOUT;
 *   int ret,len;

 *  while (poll(&fd, 1, -1) < 0) {
 *       if (errno != EINTR) {
 *           return -1;
 *       }
 *   }

 *   len = sizeof (ret);
 *   if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &ret, &len) < 0) {
 *       return -1;
 *   }
    
 *   return ret;
 *}
 */

static int tcp_check_connection(int sockfd) {
    int ret = 0;
    socklen_t len = sizeof (len);
    struct timeval tm;
    fd_set set;
    int error;

    tm.tv_sec = 3;
    tm.tv_usec = 0;
    FD_ZERO(&set);
    FD_SET(sockfd, &set);
    if (select(sockfd + 1, NULL, &set, NULL, &tm) > 0) {
        getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, (socklen_t *) & len);
        if (error == 0) {
            ret = 0;
        } else {
            ret = -1;
        }
    } else {
        ret = -1;
    }

    return ((0 != ret) ? (-1) : (0));
}

int tcp_connect(HTCPLINK lnk, const char* r_ipstr, uint16_t port_remote) {
    ncb_t *ncb;
    int retval;
    struct sockaddr_in addr_to;
    int e;
    socklen_t addrlen;
    int optval;

    if (lnk < 0 || !r_ipstr || 0 == port_remote) {
        return -1;
    }

    ncb = (ncb_t *) objrefr((objhld_t) lnk);
    if (!ncb) {
        return -1;
    }

    optval = 3;
    /* 明确定义最多尝试3次 syn 操作 */
    setsockopt(ncb->fd_, IPPROTO_TCP, TCP_SYNCNT, &optval, sizeof (optval));


    addr_to.sin_family = PF_INET;
    addr_to.sin_port = htons(port_remote);
    addr_to.sin_addr.s_addr = inet_addr(r_ipstr);
    retval = connect(ncb->fd_, (const struct sockaddr *) &addr_to, sizeof (struct sockaddr));
    e = errno;
    if (retval < 0) {
        if (e == EINPROGRESS) {
            /* 异步SOCKET可能在SYN阶段发生 EINPROGRESS 错误，此时连接应该已经建立， 但是需要检查 */
            retval = tcp_check_connection(ncb->fd_);
        }
    }

    if (retval >= 0) {
        addrlen = sizeof (addr_to);
        getpeername(ncb->fd_, (struct sockaddr *) &ncb->addr_remote_, &addrlen); /* 对端的地址信息 */
        getsockname(ncb->fd_, (struct sockaddr *) &ncb->addr_local_, &addrlen); /* 本地的地址信息 */


        retval = io_attach(ncb->fd_, ncb->hld_);
        if (retval >= 0) {
            /* 成功连接后需要确定本地和对端的地址信息 */
            addrlen = sizeof (struct sockaddr);
            getpeername(ncb->fd_, (struct sockaddr *) &ncb->addr_remote_, &addrlen); /* 对端的地址信息 */
            getsockname(ncb->fd_, (struct sockaddr *) &ncb->addr_local_, &addrlen); /* 本地的地址信息 */

            /* 成功完成 SYN, 此时可以关注数据包 */
            ncb->on_read_ = &tcp_rx;
            ncb->on_write_ = &tcp_tx;
        }
    } else {
        ncb_report_debug_information(ncb, "[TCP]failed connect remote endpoint %s:%d, err=%d\n", r_ipstr, port_remote, e);
    }

    objdefr((objhld_t) lnk);
    return retval;
}

int tcp_listen(HTCPLINK lnk, int block) {
    ncb_t *ncb;
    int retval;

    ncb = (ncb_t *) objrefr(lnk);
    if (!ncb) {
        return -1;
    }

    retval = -1;
    do {
        retval = listen(ncb->fd_, ((block > 0) ? (block) : (SOMAXCONN)));
        if (retval < 0) {
            ncb_report_debug_information(ncb, "failed syscall listen.errno=%d.\n", errno);
            break;
        }

        if (io_attach(ncb->fd_, ncb->hld_) < 0) {
            break;
        }

        /* 该 NCB 对象只可能读网络数据， 而且一定是接收链接 */
        ncb->on_read_ = &tcp_syn;
        ncb->on_write_ = NULL;
        
        retval = 0;
    } while (0);

    objdefr(lnk);
    return retval;
}

int tcp_write(HTCPLINK lnk, int cb, nis_sender_maker_t maker, void *par) {
    ncb_t *ncb;
    objhld_t hld;
    unsigned char *buffer;

    if (INVALID_HTCPLINK == lnk || cb <= 0 || cb > TCP_MAXIMUM_PACKET_SIZE || !maker) {
        return -1;
    }

    hld = (objhld_t) lnk;
    buffer = NULL;

    ncb = (ncb_t *) objrefr(hld);
    if (!ncb) {
        return -1;
    }

    do {
        if (fque_size(&ncb->packet_fifo_) >= TCP_MAXIMUM_SENDER_CACHED_CNT) {
            break;
        }

        /*必须有合法TST指定*/
        if (!(*ncb->tst_.builder_)) {
            ncb_report_debug_information(ncb, "[TCP]invalidated link object TST builder function address.\n");
            break;
        }

        /* 分配数据缓冲区 */
        buffer = (unsigned char *) malloc(cb + ncb->tst_.cb_);
        if (!buffer) {
            break;
        }

        /*构建协议头*/
        if ((*ncb->tst_.builder_)(buffer, cb) < 0) {
            break;
        }

        /*用户数据填入*/
        if ((*maker)(buffer + ncb->tst_.cb_, cb, par) < 0) {
            break;
        }

        /* 向发送队列增加一个节点, 并投递激活消息 */
        if (fque_push(&ncb->packet_fifo_, buffer, cb + ncb->tst_.cb_, NULL) < 0) {
            break;
        }
        post_task(hld, kTaskType_Write);

        objdefr(hld);
        return 0;
    } while (0);

    if (buffer) {
        free(buffer);
    }
    objdefr(hld);
    return -1;
}

int tcp_getaddr(HTCPLINK lnk, int type, uint32_t* ipv4, uint16_t* port) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;

    ncb = objrefr(hld);
    if (!ncb) return -1;

    switch (type) {
        case LINK_ADDR_LOCAL:
            *ipv4 = htonl(ncb->addr_local_.sin_addr.s_addr);
            *port = htons(ncb->addr_local_.sin_port);
            break;
        case LINK_ADDR_REMOTE:
            *ipv4 = htonl(ncb->addr_remote_.sin_addr.s_addr);
            *port = htons(ncb->addr_remote_.sin_port);
            break;
        default:
            objdefr(hld);
            return -1;
    }
    objdefr(hld);
    return 0;
}

int tcp_setopt(HTCPLINK lnk, int level, int opt, const char *val, int len) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int retval;

    ncb = objrefr(hld);
    if (!ncb) return -1;

    retval = setsockopt(ncb->fd_, level, opt, val, len);

    objdefr(hld);
    return retval;
}

int tcp_getopt(HTCPLINK lnk, int level, int opt, char *val, int *len) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int retval;

    ncb = objrefr(hld);
    if (!ncb) return -1;

    retval = getsockopt(ncb->fd_, level, opt, val, (socklen_t *) & len);

    objdefr(hld);
    return retval;
}