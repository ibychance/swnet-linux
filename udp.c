#include <stdio.h>

#include "udp.h"
#include "mxx.h"

#if !defined MAX_UDP_SIZE
#define MAX_UDP_SIZE		(MTU - (ETHERNET_P_SIZE + IP_P_SIZE + UDP_P_SIZE))
#endif

extern
void (*__notify_nshost_dbglog)(const char *logstr);

static int udp_update_opts(ncb_t *ncb) {
    static const int RECV_BUFFER_SIZE = 0xFFFF;
    static const int SEND_BUFFER_SIZE = 0xFFFF;

    if (!ncb) {
        return -1;
    }

    ncb_set_window_size(ncb, SO_RCVBUF, RECV_BUFFER_SIZE);
    ncb_set_window_size(ncb, SO_SNDBUF, SEND_BUFFER_SIZE);
    ncb_set_linger(ncb, 0, 1);
    return 0;
}

int udp_init() {
    if (ioinit() >= 0) {
        write_pool_init();
    }
    return -1;
}

void udp_uninit() {
    iouninit();
    write_pool_uninit();
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
        nis_call_ecr("Failed creat UDP socket,ip:%s,port:%u,errno:%u",l_ipstr, l_port, errno);
        return -1;
    }

    optval = 1;
    retval = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof ( optval));

    addrlocal.sin_addr.s_addr = l_ipstr ? inet_addr(l_ipstr) : INADDR_ANY;
    addrlocal.sin_family = AF_INET;
    addrlocal.sin_port = htons(l_port);
    retval = bind(fd, (struct sockaddr *) &addrlocal, sizeof ( struct sockaddr));
    if (retval < 0) {
        close(fd);
        return -1;
    }

    hld = objallo(sizeof ( ncb_t), NULL, &ncb_uninit, NULL, 0);
    if (hld < 0) {
        close(fd);
        return -1;
    }
    ncb = (ncb_t *) objrefr(hld);

    do {
        /* 基本初始化 */
        ncb_init(ncb);

        /* 复制参数 */
        ncb->nis_callback = user_callback;
        ncb->sockfd = fd;
        ncb->hld = hld;
        ncb->proto_type = kProtocolType_UDP;

        /*ET模型必须保持所有文件描述符异步进行*/
        if (setasio(fd) < 0) {
            break;
        }

        /* setsockopt */
        if (udp_update_opts(ncb) < 0) {
            break;
        }

        /*分配包缓冲区*/
        ncb->packet = (char *) malloc(UDP_BUFFER_SIZE);
        if (!ncb->packet) {
            break;
        }

        /* 处理广播/组播对象 */
        if (flag & UDP_FLAG_BROADCAST) {
            if (udp_set_boardcast(ncb, 1) < 0) {
                break;
            }
            ncb->flag |= UDP_FLAG_BROADCAST;
        } else {
            if (flag & UDP_FLAG_MULTICAST) {
                ncb->flag |= UDP_FLAG_MULTICAST;
            }
        }

        /* 获取本地地址 */
        getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen);

        /* 关注数据包 */
        ncb->ncb_read = &udp_rx;
        ncb->ncb_write = &udp_tx;
        
        /* 附加到 EPOLL */
        retval = ioatth(ncb, EPOLLIN);
        if (retval < 0) {
            break;
        }
        
        return hld;
    } while (0);

    objdefr(hld);
    objclos(hld);
    return -1;
}

void udp_destroy(HUDPLINK lnk) {
    objclos(lnk);
}

static int udp_maker(void *data, int cb, void *context) {
    if (data && cb > 0 && context) {
        memcpy(data, context, cb);
        return 0;
    }
    return -1;
}

int udp_write(HUDPLINK lnk, int cb, nis_sender_maker_t maker, void *par, const char* r_ipstr, uint16_t r_port) {
    int retval;
    ncb_t *ncb;
    struct sockaddr_in addr;
    objhld_t hld = (objhld_t) lnk;
    unsigned char *buffer;

    if ( !r_ipstr || (0 == r_port) || (cb <= 0) || (lnk < 0) || (cb > UDP_MAXIMUM_USER_DATA_SIZE)) {
        return -EINVAL;
    }

    ncb = (ncb_t *) objrefr(hld);
    if (!ncb) {
        return -ENOENT;
    }

    retval = -1;
    buffer = NULL;
    do {
        /* 发送队列过长， 无法进行发送操作 */
        if ((fque_size(&ncb->tx_fifo) >= UDP_MAXIMUM_SENDER_CACHED_CNT)) {
            break;
        }

        buffer = (unsigned char *) malloc(cb);
        if (!buffer) {
            retval = -ENOMEM;
            break;
        }

        /* 自己构造包数据 */
        if (maker) {
            if ((*maker)(buffer, cb, par) < 0) {
                break;
            }
        }else{
            if (udp_maker(buffer, cb, par) < 0){
                break;
            }
        }
        

        /* 向发送队列增加一个节点, 并投递激活消息 */
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(r_ipstr);
        addr.sin_port = htons(r_port);

        /* 向发送队列增加一个节点, 并投递激活消息 */
        if (fque_push(&ncb->tx_fifo, buffer, cb, &addr) < 0) {
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
    return retval;
}

int udp_getaddr(HUDPLINK lnk, uint32_t *ipv4, uint16_t *port) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;

    ncb = objrefr(hld);
    if (!ncb) return -1;

    *ipv4 = htonl(ncb->local_addr.sin_addr.s_addr);
    *port = htons(ncb->local_addr.sin_port);

    objdefr(hld);
    return 0;
}

int udp_setopt(HUDPLINK lnk, int level, int opt, const char *val, int len) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int retval;

    ncb = objrefr(hld);
    if (!ncb) return -1;

    retval = setsockopt(ncb->sockfd, level, opt, val, len);

    objdefr(hld);
    return retval;
}

int udp_getopt(HUDPLINK lnk, int level, int opt, char *val, int *len) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int retval;

    ncb = objrefr(hld);
    if (!ncb) return -1;

    retval = getsockopt(ncb->sockfd, level, opt, val, (socklen_t *)len);

    objdefr(hld);
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
    objhld_t hld = (objhld_t) lnk;
    int retval;

    if (lnk < 0 || !g_ipstr || 0 == g_port) {
        return -EINVAL;
    }

    ncb = objrefr(hld);
    if (!ncb) return -1;

    do {
        retval = -1;

        if (!(ncb->flag & UDP_FLAG_MULTICAST)) {
            break;
        }

        /*设置回环许可*/
        int loop = 1;
        retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const void *)&loop, sizeof (loop));
        if (retval < 0) {
            break;
        }
        
        /*加入多播组*/
        if (!ncb->mreq){
            ncb->mreq = (struct ip_mreq *)malloc(sizeof(struct ip_mreq));
        }
        ncb->mreq->imr_multiaddr.s_addr = inet_addr(g_ipstr); 
        ncb->mreq->imr_interface.s_addr = ncb->local_addr.sin_addr.s_addr; 
        retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const void *)ncb->mreq, sizeof(struct ip_mreq));
        if (retval < 0){
            break;
        }
        
    } while (0);

    objdefr(hld);
    return retval;
}

int udp_dropgrp(HUDPLINK lnk){
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int retval;
    
    if (lnk < 0){
        return -EINVAL;
    }
    
    ncb = objrefr(hld);
    if (!ncb) return -1;
    
    do{
        retval = -1;
        
        if (!(ncb->flag & UDP_FLAG_MULTICAST) || !ncb->mreq) {
            break;
        }
        
        /*还原回环许可*/
        int loop = 0;
        retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const void *)&loop, sizeof (loop));
        if (retval < 0) {
            break;
        }
        
        /*离开多播组*/
        retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const void *)ncb->mreq, sizeof(struct ip_mreq));
        
    }while(0);
    
    objdefr(hld);
    return retval;
}