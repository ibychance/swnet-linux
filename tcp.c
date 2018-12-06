#include <stdio.h>
#include <time.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "tcp.h"
#include "mxx.h"

/*
 *  kernel status of tcpi_state
 *  defined in /usr/include/netinet/tcp.h
 *  enum
 *  {
 *    TCP_ESTABLISHED = 1,
 *    TCP_SYN_SENT,
 *    TCP_SYN_RECV,
 *    TCP_FIN_WAIT1,
 *    TCP_FIN_WAIT2,
 *    TCP_TIME_WAIT,
 *    TCP_CLOSE,
 *    TCP_CLOSE_WAIT,
 *    TCP_LAST_ACK,
 *    TCP_LISTEN,
 *    TCP_CLOSING 
 *  };
 */
const char *TCP_KERNEL_STATE_NAME[] = {
    "TCP_UNDEFINED", "TCP_ESTABLISHED", "TCP_SYN_SENT", "TCP_SYN_RECV", "TCP_FIN_WAIT1", "TCP_FIN_WAIT2", 
    "TCP_TIME_WAIT", "TCP_CLOSE", "TCP_CLOSE_WAIT", "TCP_LAST_ACK", "TCP_LISTEN", "TCP_CLOSING"
};

void tcp_update_opts(ncb_t *ncb) {
    if (ncb) {
        ncb_set_window_size(ncb, SO_RCVBUF, TCP_BUFFER_SIZE);
        ncb_set_window_size(ncb, SO_SNDBUF, TCP_BUFFER_SIZE);
        
        /* atomic keepalive */
        tcp_set_keepalive(ncb, 1);
        tcp_set_keepalive_value(ncb, 30, 5, 6);

        /* disable the Nginx algorithm */
        tcp_set_nodelay(ncb, 1);
    }
}

/* tcp impls */
int tcp_init() {
    if (ioinit() >= 0) {
        write_pool_init();
    }
    return 0;
}

void tcp_uninit() {
    iouninit();
    write_pool_uninit();
}

HTCPLINK tcp_create(tcp_io_callback_t user_callback, const char* l_ipstr, uint16_t l_port) {
    int fd;
    struct sockaddr_in addrlocal;
    int retval;
    int optval;
    ncb_t *ncb;
    objhld_t hld = -1;

    if (!user_callback) {
        return INVALID_HTCPLINK;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (l_ipstr) {
            nis_call_ecr("nshost.tcp.create: file descriptor create failed,%s:%u,errno:%u",l_ipstr, l_port, errno);
        } else {
            nis_call_ecr("nshost.tcp.create: file descriptor create failed, 0.0.0.0:%u,errno:%u", l_port, errno);
        }
        
        return -1;
    }

    /* allow port reuse */
    optval = 1;
    retval = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof ( optval));

    /* binding address, and then allocate NCB object */
    addrlocal.sin_addr.s_addr = l_ipstr ? inet_addr(l_ipstr) : INADDR_ANY;
    addrlocal.sin_family = AF_INET;
    addrlocal.sin_port = htons(l_port);
    retval = bind(fd, (struct sockaddr *) &addrlocal, sizeof ( struct sockaddr));
    if (retval < 0) {
        nis_call_ecr("nshost.tcp.create: bind sockaddr failed, %s:%u, errno:%d.\n", l_ipstr, l_port, errno);
        close(fd);
        return -1;
    }

    hld = objallo(sizeof(ncb_t), NULL, &ncb_uninit, NULL, 0);
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
        ncb->hld = hld;
        ncb->sockfd = fd;
        ncb->proto_type = kProtocolType_TCP;
        ncb->nis_callback = user_callback;
        memcpy(&ncb->local_addr, &addrlocal, sizeof (addrlocal));

        /* acquire save TCP Info and adjust linger in the creation phase. */
        ncb_set_linger(ncb, 0, 0);

        /* allocate normal TCP package */
        ncb->packet = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb->packet) {
            break;
        }

        /* zeroization protocol head*/
        ncb->u.tcp.rx_parse_offset = 0;
        ncb->u.tcp.rx_buffer = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb->u.tcp.rx_buffer) {
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
    int retval;

    if (lnk < 0 || !tst) {
        return -1;
    }
    
    ncb = (ncb_t *) objrefr((objhld_t) lnk);
    if (!ncb) {
        return -1;
    }

    /* size of tcp template must be less or equal to 32 bytes */
    if (tst->cb_ > TCP_MAXIMUM_TEMPLATE_SIZE) {
        retval = RE_ERROR(EINVAL);
    } else {
        /* allows change the tst info that is already existed.  */
        ncb->u.tcp.template.cb_ = tst->cb_;
        ncb->u.tcp.template.builder_ = tst->builder_;
        ncb->u.tcp.template.parser_ = tst->parser_;
        retval = 0;
    }

    objdefr((objhld_t) lnk);
    return retval;
}

int tcp_gettst(HTCPLINK lnk, tst_t *tst) {
    ncb_t *ncb;

    if (lnk < 0 || !tst) {
        return -1;
    }

    ncb = (ncb_t *) objrefr((objhld_t) lnk);
    if (!ncb) {
        return -1;
    }

    tst->cb_ = ncb->u.tcp.template.cb_;
    tst->builder_ = ncb->u.tcp.template.builder_;
    tst->parser_ = ncb->u.tcp.template.parser_;
    objdefr((objhld_t) lnk);
    return 0;
}

/*
 * Object destruction operations may be intended to interrupt some blocking operations. just like @tcp_connect
 * so,close the file descriptor directly, destroy the object by the smart pointer.
 */
void tcp_destroy(HTCPLINK lnk) {
    ncb_t *ncb;

    if (tcp_init() < 0) {
        return;
    }

    /* it should be the last reference operation of this object, no matter how many ref-count now. */
    ncb = objreff((objhld_t) lnk);
    if (ncb) {
        nis_call_ecr("nshost.tcp.destroy: link %lld order to destroy", ncb->hld);
        ioclose(ncb);
        objdefr((objhld_t) lnk);
    }
}

#if 0

/* <tcp_check_connection_bypoll> */
static int __tcp_check_connection_bypoll(int sockfd) {
    struct pollfd pofd;
    socklen_t len;
    int error;

    pofd.fd = sockfd;
    pofd.events = POLLOUT;

    while(poll(&pofd, 1, -1) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }

    len = sizeof (error);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        return -1;
    }

    return 0;
}

/* <tcp_check_connection_byselect> */
static int __tcp_check_connection(int sockfd) {
    int retval;
    socklen_t len;
    struct timeval timeo;
    fd_set rset, wset;
    int error;
    int nfd;

    /* 3 seconds as maximum wait time long*/
    timeo.tv_sec = 3;
    timeo.tv_usec = 0;

    FD_ZERO(&rset);
    FD_SET(sockfd, &rset);
    wset = rset;

    retval = -1;
    len = sizeof (error);
    do {
        
        /* The nfds argument specifies the range of descriptors to be tested. 
         * The first nfds descriptors shall be checked in each set; 
         * that is, the descriptors from zero through nfds-1 in the descriptor sets shall be examined. 
         */
        nfd = select(sockfd + 1, &rset, &wset, NULL, &timeo);
        if ( nfd <= 0) {
            break;
        }

        if (FD_ISSET(sockfd, &rset) || FD_ISSET(sockfd, &wset)) {
            retval = getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, (socklen_t *) & len);
            if ( retval < 0) {
                break;
            }
            retval = error;
        }
    } while (0);

    return retval;
}

#endif

int tcp_connect(HTCPLINK lnk, const char* r_ipstr, uint16_t r_port) {
    ncb_t *ncb;
    int retval;
    struct sockaddr_in addr_to;
    int e;
    socklen_t addrlen;
    int optval;
    struct tcp_info ktcp;

    if (lnk < 0 || !r_ipstr || 0 == r_port || tcp_init() < 0) {
        return -1;
    }

    ncb = (ncb_t *) objrefr((objhld_t) lnk);
    if (!ncb) {
        return -1;
    }

    do {
        retval = -1;

        /* get the socket status of tcp_info to check the socket tcp statues */
        if (tcp_save_info(ncb, &ktcp) >= 0) {
            if (ktcp.tcpi_state != TCP_CLOSE) {
                nis_call_ecr("nshost.tcp.connect:state illegal,link:%lld, kernel states %s.", lnk, TCP_KERNEL_STATE_NAME[ktcp.tcpi_state]);
                break;
            }
        }

        /* try no more than 3 times of tcp::syn */
        optval = 3;
        setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_SYNCNT, &optval, sizeof (optval));

        addr_to.sin_family = PF_INET;
        addr_to.sin_port = htons(r_port);
        addr_to.sin_addr.s_addr = inet_addr(r_ipstr);

        /* syscall @connect can be interrupted by other signal. */
        do {
            retval = connect(ncb->sockfd, (const struct sockaddr *) &addr_to, sizeof (struct sockaddr));
            e = errno;
        } while((e == EINTR) && (retval < 0));

        if (retval < 0) {
            /* if this socket is already connected, or it is in listening states, sys-call failed with error EISCONN  */
            nis_call_ecr("nshost.tcp.connect:fatal syscall, link:%lld, %s:%u, err=%d", lnk, r_ipstr, r_port, e);
            break;
        }

        /* set other options */
        tcp_update_opts(ncb);
        
        /* save address information after connect successful */
        addrlen = sizeof (addr_to);
        getpeername(ncb->sockfd, (struct sockaddr *) &ncb->remot_addr, &addrlen); /* remote address information */
        getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen); /* local address information */

        /* set handler function pointer to Rx/Tx */
        ncb->ncb_read = &tcp_rx;
        ncb->ncb_write = &tcp_tx;

        /* ensuer all file descriptor in asynchronous mode, 
           and than, queue object into epoll manager */
        retval = setasio(ncb->sockfd);
        if (retval >= 0) {
            retval = ioatth(ncb, EPOLLIN);
            if (retval >= 0) {
                ncb_post_connected(ncb);
            }
        }
    }while( 0 );

    objdefr((objhld_t) lnk);
    return retval;
}

int tcp_connect2(HTCPLINK lnk, const char* r_ipstr, uint16_t r_port) {
    ncb_t *ncb;
    int retval;
    int e;
    int optval;
    struct tcp_info ktcp;

    if (lnk < 0 || !r_ipstr || 0 == r_port || tcp_init() < 0) {
        return -1;
    }

    ncb = (ncb_t *) objrefr(lnk);
    if (!ncb) {
        return -1;
    }
    
    do {
        retval = -1;

        /* get the socket status of tcp_info to check the socket tcp statues */
        if (tcp_save_info(ncb, &ktcp) >= 0) {
            if (ktcp.tcpi_state != TCP_CLOSE) {
                nis_call_ecr("nshost.tcp.connect2:state illegal,link:%lld, kernel states %s.", lnk, TCP_KERNEL_STATE_NAME[ktcp.tcpi_state]);
                break;
            }
        }

        /* try no more than 3 times of tcp::syn */
        optval = 3;
        setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_SYNCNT, &optval, sizeof (optval));

        ncb->ncb_write = &tcp_tx_syn;
        
        /* queue object into epoll manage befor syscall @connect,
           epoll_wait will get a EPOLLOUT signal when syn success.
           so, file descriptor must be set to asynchronous now. */
        if (setasio(ncb->sockfd) < 0) {
            break;
        }
        if (ioatth(ncb, EPOLLOUT) < 0) {
            break;
        }

        ncb->remot_addr.sin_family = PF_INET;
        ncb->remot_addr.sin_port = htons(r_port);
        ncb->remot_addr.sin_addr.s_addr = inet_addr(r_ipstr);

        retval = connect(ncb->sockfd, (const struct sockaddr *) &ncb->remot_addr, sizeof (struct sockaddr));
        if (retval < 0) {
            e = errno;
            if (e != EINPROGRESS) {
                break;
            }
        }

        retval = 0;
    } while (0);

    objdefr((objhld_t) lnk);
    return retval;
}

int tcp_listen(HTCPLINK lnk, int block) {
    ncb_t *ncb;
    int retval;
    struct tcp_info ktcp;

    if (lnk < 0 || tcp_init() < 0) {
        return -1;
    }

    ncb = (ncb_t *) objrefr(lnk);
    if (!ncb) {
        return -1;
    }

    do {
        retval = -1;

        /* get the socket status of tcp_info to check the socket tcp statues */
        if (tcp_save_info(ncb, &ktcp) >= 0) {
            if (ktcp.tcpi_state != TCP_CLOSE) {
                nis_call_ecr("nshost.tcp.listen:state illegal,link:%lld, kernel states %s.", lnk, TCP_KERNEL_STATE_NAME[ktcp.tcpi_state]);
                break;
            }
        }

        /* '/proc/sys/net/core/somaxconn' in POSIX.1 this value default to 128
           so,for ensure high concurrency performance in the establishment phase of the TCP connection,
           we will ignore the @block argument and use macro SOMAXCONN which defined in /usr/include/bits/socket.h anyway
         */
        retval = listen(ncb->sockfd, ((0 == block) || (block > SOMAXCONN)) ? SOMAXCONN : block);
        if (retval < 0) {
            nis_call_ecr("nshost.tcp.listen:fatal syscall,err=%d", errno);
            break;
        }

        /* file descriptor must set to asynchronous mode befor accept */
        retval = setasio(ncb->sockfd);
        if (retval < 0){
            break;
        }

        /* this NCB object is readonly， and it must be used for accept */
        ncb->ncb_read = &tcp_syn;
        ncb->ncb_write = NULL;
        
        if (ioatth(ncb, EPOLLIN) < 0) {
            break;
        }
        retval = 0;
    } while (0);

    objdefr(lnk);
    return retval;
}

static 
int tcp_maker(void *data, int cb, const void *context) {
    if (data && cb > 0 && context) {
        memcpy(data, context, cb);
        return 0;
    }
    return -1;
}

int tcp_write(HTCPLINK lnk, int cb, nis_sender_maker_t maker, const void *par) {
    ncb_t *ncb;
    objhld_t hld;
    unsigned char *buffer;
    nis_sender_maker_t amaker;
    int packet_length;
    struct tcp_info ktcp;

    if ( lnk < 0 || cb <= 0 || cb > TCP_MAXIMUM_PACKET_SIZE || tcp_init() < 0 ) {
        return -1;
    }

    hld = (objhld_t) lnk;
    buffer = NULL;

    ncb = (ncb_t *) objrefr(hld);
    if (!ncb) {
        return -1;
    }

    do {
        /* the calling thread is likely to occur as follows:
         * immediately call @tcp_write after creation, but no connection established and no listening has yet been taken
         * in this situation, @wpool::run_task maybe take a task, but @ncb->ncb_write is ineffectiveness.application may crashed.
         * Judging these two parameters to ensure their effectiveness
         */
        if (!ncb->ncb_write || !ncb->ncb_read) {
            break;
        }

        /* get the socket status of tcp_info to check the socket tcp statues */
        if (tcp_save_info(ncb, &ktcp) >= 0) {
            if (ktcp.tcpi_state != TCP_ESTABLISHED) {
                nis_call_ecr("nshost.tcp.write:state illegal,link:%lld, kernel states %s.", lnk, TCP_KERNEL_STATE_NAME[ktcp.tcpi_state]);
                break;
            }
        }

        /* to large length of current queue,no more message can be post */
        if (fque_size(&ncb->tx_fifo) >= TCP_MAXIMUM_SENDER_CACHED_CNT) {
            break;
        }

        /* user data filler */
        amaker = maker;
        if (!maker) {
            amaker = &tcp_maker;
        }

        /* if template.builder is specified then use it, otherwise, indicate the packet size by input parameter @cb */
        if (!(*ncb->u.tcp.template.builder_)) {
            packet_length = cb;
            buffer = (unsigned char *) malloc(packet_length);
            if (!buffer) {
                break;
            }

             /* fill user data seg */
            if ((*amaker)(buffer, cb, par) < 0){
                break;
            }
        } else {
            packet_length = cb + ncb->u.tcp.template.cb_;
            buffer = (unsigned char *) malloc(packet_length);
            if (!buffer) {
                break;
            }

            /* build protocol head */
            if ((*ncb->u.tcp.template.builder_)(buffer, cb) < 0) {
                break;
            }

            /* fill user data seg */
            if ((*amaker)(buffer + ncb->u.tcp.template.cb_, cb, par) < 0) {
                break;
            }
        }

        /* push message to the tail of the queue, awaken write thread */
        if (fque_push(&ncb->tx_fifo, buffer, cb + ncb->u.tcp.template.cb_, NULL) < 0) {
            break;
        }
        post_write_task(hld, kTaskType_TxTest);

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
    if (!ncb) {
        return -1;
    }

    switch (type) {
        case LINK_ADDR_LOCAL:
            *ipv4 = htonl(ncb->local_addr.sin_addr.s_addr);
            *port = htons(ncb->local_addr.sin_port);
            break;
        case LINK_ADDR_REMOTE:
            *ipv4 = htonl(ncb->remot_addr.sin_addr.s_addr);
            *port = htons(ncb->remot_addr.sin_port);
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
    if (!ncb) {
        return -1;
    }

    retval = setsockopt(ncb->sockfd, level, opt, (const void *) val, (socklen_t) len);

    objdefr(hld);
    return retval;
}

int tcp_getopt(HTCPLINK lnk, int level, int opt, char *__restrict val, int *len) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int retval;

    ncb = objrefr(hld);
    if (!ncb) {
        return -1;
    }

    retval = getsockopt(ncb->sockfd, level, opt, (void * __restrict)val, (socklen_t *) len);

    objdefr(hld);
    return retval;
}

int tcp_save_info(ncb_t *ncb, struct tcp_info *ktcp) {
    socklen_t len = sizeof (struct tcp_info);

    if (!ktcp) {
        return -1;
    }

    return getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_INFO, (void * __restrict)ktcp, &len);
}

int tcp_setmss(ncb_t *ncb, int mss) {
    if (ncb && mss > 0) {
        return setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_MAXSEG, (const void *) &mss, sizeof (mss));
    }

    return RE_ERROR(EINVAL);
}

int tcp_getmss(ncb_t *ncb) {
    if (ncb) {
        socklen_t lenmss = sizeof (ncb->u.tcp.mss);
        return getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_MAXSEG, (void *__restrict) & ncb->u.tcp.mss, &lenmss);
    }
    return RE_ERROR(EINVAL);
}

int tcp_set_nodelay(ncb_t *ncb, int set) {
    if (ncb) {
        return setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_NODELAY, (const void *) &set, sizeof ( set));
    }

    return RE_ERROR(EINVAL);
}

int tcp_get_nodelay(ncb_t *ncb, int *set) {
    if (ncb && set) {
        socklen_t optlen = sizeof (int);
        return getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_NODELAY, (void *__restrict)set, &optlen);
    }
    return RE_ERROR(EINVAL);
}

int tcp_set_cork(ncb_t *ncb, int set) {
    if (ncb) {
        return setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_CORK, (const void *) &set, sizeof ( set));
    }

    return RE_ERROR(EINVAL);
}

int tcp_get_cork(ncb_t *ncb, int *set) {
    if (ncb && set) {
        socklen_t optlen = sizeof (int);
        return getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_CORK, (void *__restrict)set, &optlen);
    }
    return RE_ERROR(EINVAL);
}

int tcp_set_keepalive(ncb_t *ncb, int enable) {
    if (ncb) {
        return setsockopt(ncb->sockfd, SOL_SOCKET, SO_KEEPALIVE, (const char *) &enable, sizeof ( enable));
    }
    return RE_ERROR(EINVAL);
}

int tcp_get_keepalive(ncb_t *ncb, int *enabled){
    if (ncb && enabled) {
        socklen_t optlen = sizeof(int);
        return getsockopt(ncb->sockfd, SOL_SOCKET, SO_KEEPALIVE, (void *__restrict)enabled, &optlen);
    }
    return RE_ERROR(EINVAL);
}

int tcp_set_keepalive_value(ncb_t *ncb, int idle, int interval, int probes) {
    int enabled;
    if (tcp_get_keepalive(ncb, &enabled) < 0) {
        return -1;
    }

    if (!enabled) {
        return -1;
    }

    do {
        /* 如果在这个时间内没有数据往来， 则进行心跳检查 */
        if (setsockopt(ncb->sockfd, SOL_TCP, TCP_KEEPIDLE, (void *)&idle, sizeof(idle)) < 0) {
            break;
        }

        /* 心跳包的检查间隔 */
        if (setsockopt(ncb->sockfd, SOL_TCP, TCP_KEEPINTVL, (void *)&interval, sizeof(interval)) < 0) {
            break;
        }

        /* 允许心跳失败的次数 */
        if (setsockopt(ncb->sockfd, SOL_TCP, TCP_KEEPCNT, (void *)&probes, sizeof(probes)) < 0) {
            break;
        }

        return 0;
    }while( 0 );
    
    return -1;
}

int tcp_get_keepalive_value(ncb_t *ncb,int *idle, int *interval, int *probes) {
    int enabled;
    socklen_t optlen;

    if (tcp_get_keepalive(ncb, &enabled) < 0) {
        return -1;
    }

    if (!enabled) {
        return -1;
    }

    do {
        optlen = sizeof(int);

        if (idle) {
            if (getsockopt(ncb->sockfd, SOL_TCP, TCP_KEEPIDLE, (void *__restrict)idle, &optlen) < 0) {
                break;
            }
        }

        if (interval) {
            if (getsockopt(ncb->sockfd, SOL_TCP, TCP_KEEPINTVL, (void *__restrict)interval, &optlen) < 0) {
                break;
            }
        }

        if (probes) {
            if (getsockopt(ncb->sockfd, SOL_TCP, TCP_KEEPCNT, (void *__restrict)probes, &optlen) < 0) {
                break;
            }
        }

        return 0;
    }while( 0 );
    
    return -1;
}