#include "udp.h"

#include "mxx.h"
#include "fifo.h"
#include "io.h"
#include "wpool.h"

static int __udp_update_opts(ncb_t *ncb) {
    static const int RECV_BUFFER_SIZE = 0xFFFF;
    static const int SEND_BUFFER_SIZE = 0xFFFF;

    if (!ncb) {
        return -EINVAL;
    }

    ncb_set_window_size(ncb, SO_RCVBUF, RECV_BUFFER_SIZE);
    ncb_set_window_size(ncb, SO_SNDBUF, SEND_BUFFER_SIZE);
    ncb_set_linger(ncb, 0, 1);
    return 0;
}

int udp_init() {
    if (ioinit() >= 0) {
        return wp_init();
    }
    return -1;
}

void udp_uninit() {
    iouninit();
    wp_uninit();
}

HUDPLINK udp_create(udp_io_callback_t user_callback, const char* l_ipstr, uint16_t l_port, int flag) {
    int fd;
    struct sockaddr_in addrlocal;
    int retval;
    int optval;
    objhld_t hld;
    socklen_t addrlen;
    ncb_t *ncb;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        if (l_ipstr) {
            nis_call_ecr("nshost.udp.create: file descriptor create failed,%s:%u,errno:%u",l_ipstr, l_port, errno);
        } else {
            nis_call_ecr("nshost.udp.create: file descriptor create failed, 0.0.0.0:%u,errno:%u", l_port, errno);
        }
        return -1;
    }

    optval = 1;
    retval = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof ( optval));

    addrlocal.sin_addr.s_addr = l_ipstr ? inet_addr(l_ipstr) : INADDR_ANY;
    addrlocal.sin_family = AF_INET;
    addrlocal.sin_port = htons(l_port);
    retval = bind(fd, (struct sockaddr *) &addrlocal, sizeof ( struct sockaddr));
    if (retval < 0) {
        nis_call_ecr("nshost.udp.create: bind sockaddr failed, %s:%u, errno:%d.\n", l_ipstr, l_port, errno);
        close(fd);
        return -1;
    }

    hld = objallo(sizeof ( ncb_t), NULL, &ncb_uninit, NULL, 0);
    if (hld < 0) {
        nis_call_ecr("nshost.udp.create: failed allocate inner object");
        close(fd);
        return -1;
    }
    ncb = (ncb_t *) objrefr(hld);
    assert(ncb);

    do {
        ncb_init(ncb);

        /* copy initialize parameters */
        ncb->nis_callback = user_callback;
        ncb->sockfd = fd;
        ncb->hld = hld;
        ncb->proto_type = kProtocolType_UDP;

        /* must keep all file descriptor in asynchronous mode with ET mode */
        if (setasio(fd) < 0) {
            break;
        }

        /* setsockopt */
        if (__udp_update_opts(ncb) < 0) {
            break;
        }

        /* allocate buffer for normal packet */
        ncb->packet = (char *) malloc(UDP_BUFFER_SIZE);
        if (!ncb->packet) {
            retval = -ENOMEM;
            break;
        }

        /* extension of broadcast/multicast */
        if (flag & UDP_FLAG_BROADCAST) {
            if (udp_set_boardcast(ncb, 1) < 0) {
                break;
            }
            ncb->u.udp.flag |= UDP_FLAG_BROADCAST;
        } else {
            if (flag & UDP_FLAG_MULTICAST) {
                ncb->u.udp.flag |= UDP_FLAG_MULTICAST;
            }
        }

        /* get local address info */
        addrlen = sizeof(ncb->local_addr);
        getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen);

        /* set data handler function pointer for Rx/Tx */
        ncb->ncb_read = &udp_rx;
        ncb->ncb_write = &udp_tx;
        
        /* attach to epoll */
        retval = ioatth(ncb, EPOLLIN);
        if (retval < 0) {
            break;
        }
        
        objdefr(hld);
        return hld;
    } while (0);

    objdefr(hld);
    objclos(hld);
    return -1;
}

void udp_destroy(HUDPLINK lnk) {
    ncb_t *ncb;

    /* it should be the last reference operation of this object no matter how many ref-count now. */
    ncb = objreff(lnk);
    if (ncb) {
        nis_call_ecr("nshost.udp.destroy: link %lld order to destroy", ncb->hld);
        ioclose(ncb);
        objdefr(lnk);
    }
}

static int udp_maker(void *data, int cb, const void *context) {
    if (data && cb > 0 && context) {
        memcpy(data, context, cb);
        return 0;
    }
    return -1;
}

int udp_write(HUDPLINK lnk, int cb, nis_sender_maker_t maker, const void *par, const char* r_ipstr, uint16_t r_port) {
    int retval;
    ncb_t *ncb;
    unsigned char *buffer;
    struct tx_node *node;

    if ( !r_ipstr || (0 == r_port) || (cb <= 0) || (lnk < 0) || (cb > MAX_UDP_SIZE)) {
        return -EINVAL;
    }

    retval = -1;
    buffer = NULL;
    node = NULL;

    ncb = (ncb_t *) objrefr(lnk);
    if (!ncb) {
        return -ENOENT;
    }

    do {
        if (ncb->proto_type != kProtocolType_UDP) {
            retval = -EPROTOTYPE;
            break;
        }

        buffer = (unsigned char *) malloc(cb);
        if (!buffer) {
            retval = -ENOMEM;
            break;
        }

        if (maker) {
            if ((*maker)(buffer, cb, par) < 0) {
                nis_call_ecr("nshost.udp.write:fatal call amaker");
                break;
            }
        }else{
            if (udp_maker(buffer, cb, par) < 0) {
                nis_call_ecr("nshost.udp.write:fatal call amaker");
                break;
            }
        }

        node = (struct tx_node *) malloc(sizeof (struct tx_node));
        if (!node) {
            retval = -ENOMEM;
            break;
        }
        memset(node, 0, sizeof(struct tx_node));
        node->data = buffer;
        node->wcb = cb;
        node->offset = 0;
        node->udp_target.sin_family = AF_INET;
        node->udp_target.sin_addr.s_addr = inet_addr(r_ipstr);
        node->udp_target.sin_port = htons(r_port);

        if (!fifo_is_blocking(ncb)) {
            retval = udp_txn(ncb, node);

            /* 
             * the return value means direct failed when it equal to -1 or success when it greater than zero.
             * in these case, destroy memory resource outside loop, no matter what the actually result it is.
             */
            if (-EAGAIN != retval) {
                break; 
            }
        }

        /* 
         * when the IO blocking is existed, we can't send data immediately,
         * only way to handler this situation is queued data into @wpool.
         * otherwise, the wrong operation may broken the output sequence 
         *
         * in case of -EAGAIN return by @udp_txn, means the write operation cannot be complete right now,
         * insert @node into the tail of @fifo queue, be careful, in this case, memory of @buffer and @node cannot be destroy until asynchronous completed
         *
         * just insert @node into tail of @fifo queue,  awaken write thread is not necessary.
         * don't worry about the task thread notify, when success calling to @ncb_set_blocking, ensure that the @EPOLLOUT event can being captured by IO thread 
         */
        fifo_queue(ncb, node);
        objdefr(lnk);
        return 0;
    } while (0);

    if (buffer) {
        free(buffer);
    }

    if (node) {
        free(node);
    }

    objdefr(lnk);
    return retval;
}

int udp_getaddr(HUDPLINK lnk, uint32_t *ipv4, uint16_t *port) {
    ncb_t *ncb;
    int retval;

    ncb = objrefr(lnk);
    if (!ncb) {
        return -ENOENT;
    }

    if (ncb->proto_type == kProtocolType_UDP) {
        if (ipv4) {
            *ipv4 = htonl(ncb->local_addr.sin_addr.s_addr);
        }
        if (port) {
            *port = htons(ncb->local_addr.sin_port);
        }
        retval = 0;
    } else {
        retval = -EPROTOTYPE;
    }

    objdefr(lnk);
    return retval;
}

int udp_setopt(HUDPLINK lnk, int level, int opt, const char *val, int len) {
    ncb_t *ncb;
    int retval;

    ncb = objrefr(lnk);
    if (!ncb) {
        return -ENOENT;
    }

    retval = -EPROTOTYPE;
    if (ncb->proto_type == kProtocolType_UDP) {
        retval = setsockopt(ncb->sockfd, level, opt, val, len);
    }

    objdefr(lnk);
    return retval;
}

int udp_getopt(HUDPLINK lnk, int level, int opt, char *val, int *len) {
    ncb_t *ncb;
    int retval;

    ncb = objrefr(lnk);
    if (!ncb) {
        return -ENOENT;
    }

    retval = -EPROTOTYPE;
    if (ncb->proto_type == kProtocolType_UDP) {
        retval = getsockopt(ncb->sockfd, level, opt, val, (socklen_t *)len);
    }

    objdefr(lnk);
    return retval;
}

int udp_set_boardcast(ncb_t *ncb, int enable) {
    if (ncb) {
        return setsockopt(ncb->sockfd, SOL_SOCKET, SO_BROADCAST, (const void *) &enable, sizeof (enable));
    }
    return -EINVAL;
}

int udp_get_boardcast(ncb_t *ncb, int *enabled) {
    if (ncb && enabled) {
        socklen_t optlen = sizeof (int);
        return getsockopt(ncb->sockfd, SOL_SOCKET, SO_BROADCAST, (void * __restrict)enabled, &optlen);
    }
    return -EINVAL;
}

/*
 * 组播报文的目的地址使用D类IP地址， D类地址不能出现在IP报文的源IP地址字段。单播数据传输过程中，一个数据包传输的路径是从源地址路由到目的地址，利用“逐跳”的原理[路由选择]在IP网络中传输。
 * 然而在ip组播环中，数据包的目的地址不是一个，而是一组，形成组地址。所有的信息接收者都加入到一个组内，并且一旦加入之后，流向组地址的数据立即开始向接收者传输，组中的所有成员都能接收到数据包。
 * 组播组中的成员是动态的，主机可以在任何时刻加入和离开组播组。
 *         用同一个IP多播地址接收多播数据包的所有主机构成了一个主机组，也称为多播组。一个多播组的成员是随时变动的，一台主机可以随时加入或离开多播组，多播组成员的数目和所在的地理位置也不受限制，一台主机也可以属于几个多播组。
 * 此外，不属于某一个多播组的主机也可以向该多播组发送数据包。  
 * 
 * 组播地址
 * 组播组可以是永久的也可以是临时的。组播组地址中，有一部分由官方分配的，称为永久组播组。
 * 永久组播组保持不变的是它的ip地址，组中的成员构成可以发生变化。永久组播组中成员的数量都可以是任意的，甚至可以为零。那些没有保留下来供永久组播组使用的ip组播地址，可以被临时组播组利用。
 *      224.0.0.0～224.0.0.255为预留的组播地址（永久组地址），地址224.0.0.0保留不做分配，其它地址供路由协议使用
 *      224.0.1.0～224.0.1.255是公用组播地址，可以用于Internet
 *      224.0.2.0～238.255.255.255为用户可用的组播地址（临时组地址），全网范围内有效
 *      239.0.0.0～239.255.255.255为本地管理组播地址，仅在特定的本地范围内有效
 * 
 *  组播是一对多的传输方式，其中有个组播组的 概念，发送端将数据向一个组内发送，网络中的路由器通过底层的IGMP协议自动将数据发送到所有监听这个组的终端。
 *  至于广播则和组播有一些相似，区别是路由器向子网内的每一个终端都投递一份数据包，不论这些终端是否乐于接收该数据包。UDP广播只能在内网（同一网段）有效，而组播可以较好实现跨网段群发数据。
 *   UDP组播是采用的无连接,数据报的连接方式，所以是不可靠的。也就是数据能不能到达接受端和数据到达的顺序都是不能保证的。但是由于UDP不用保证数据 的可靠性，所有数据的传送效率是很快的。
 */
int udp_joingrp(HUDPLINK lnk, const char *g_ipstr, uint16_t g_port) {
    ncb_t *ncb;
    int retval;

    if (lnk < 0 || !g_ipstr || 0 == g_port) {
        return -EINVAL;
    }

    ncb = objrefr(lnk);
    if (!ncb) {
        return -ENOENT;
    }

    do {
        retval = -1;

        if (ncb->proto_type != kProtocolType_UDP) {
            retval = -EPROTOTYPE;
            break;
        }

        if (!(ncb->u.udp.flag & UDP_FLAG_MULTICAST)) {
            break;
        }

        /* set permit for loopback */
        int loop = 1;
        retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const void *)&loop, sizeof (loop));
        if (retval < 0) {
            break;
        }
        
        /* insert into multicast group */
        if (!ncb->u.udp.mreq){
            ncb->u.udp.mreq = (struct ip_mreq *)malloc(sizeof(struct ip_mreq));
        }
        ncb->u.udp.mreq->imr_multiaddr.s_addr = inet_addr(g_ipstr); 
        ncb->u.udp.mreq->imr_interface.s_addr = ncb->local_addr.sin_addr.s_addr; 
        retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const void *)ncb->u.udp.mreq, sizeof(struct ip_mreq));
        if (retval < 0){
            break;
        }
        
    } while (0);

    objdefr(lnk);
    return retval;
}

int udp_dropgrp(HUDPLINK lnk){
    ncb_t *ncb;
    int retval;
    
    ncb = objrefr(lnk);
    if (!ncb) {
        return -ENOENT;
    }

    do{
        retval = -1;

        if (ncb->proto_type != kProtocolType_UDP) {
            retval = -EPROTOTYPE;
            break;
        }
        
        if (!(ncb->u.udp.flag & UDP_FLAG_MULTICAST) || !ncb->u.udp.mreq) {
            break;
        }
        
        /* reduction permit for loopback */
        int loop = 0;
        retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const void *)&loop, sizeof (loop));
        if (retval < 0) {
            break;
        }
        
        /* leave multicast group */
        retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const void *)ncb->u.udp.mreq, sizeof(struct ip_mreq));
        
    }while(0);
    
    objdefr(lnk);
    return retval;
}
