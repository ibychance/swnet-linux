#include <stdio.h>

#include "udp.h"

extern
void (*__notify_nshost_dbglog)(const char *logstr);

static int udp_update_opts(ncb_t *ncb) {
    struct linger lgr;
    int fd;
    static const int RECV_BUFFER_SIZE = 0xFFFF;
    static const int SEND_BUFFER_SIZE = 0xFFFF;

    if (!ncb) {
        return -1;
    }
    fd = ncb->sockfd;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *) &RECV_BUFFER_SIZE, sizeof ( RECV_BUFFER_SIZE)) < 0) {
        ncb_report_debug_information(ncb, "failed to set SO_RCVBUF, errno=%d.\n", errno);
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char *) &SEND_BUFFER_SIZE, sizeof ( SEND_BUFFER_SIZE)) < 0) {
        ncb_report_debug_information(ncb, "failed to set SO_SNDBUF, errno=%d.\n", errno);
        return -1;
    }

    lgr.l_onoff = 0;
    lgr.l_linger = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, (char *) &lgr, sizeof ( struct linger)) < 0) {
        ncb_report_debug_information(ncb, "failed to set SO_LINGER, errno=%d.\n", errno);
        return -1;
    }
    return 0;
}

int udp_init() {
    if (ioinit() >= 0) {
        read_pool_init();
        write_pool_init();
    }
    return -1;
}

void udp_uninit() {
    iouninit();
    read_pool_uninit();
    write_pool_uninit();
}

HUDPLINK udp_create(udp_io_callback_t user_callback, const char* l_ipstr, uint16_t l_port, int flag) {
    int fd;
    struct sockaddr_in addrlocal;
    int retval;
    int optval;
    objhld_t hld;
    socklen_t addrlen;
    int enable_broadcast;
    ncb_t *ncb;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("failed create services socket.errno=%d.\n", errno);
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

    hld = objallo(sizeof ( ncb_t), &objentry, &ncb_uninit, NULL, 0);
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

        /* 处理广播对象 */
        ncb->flag = flag;
        if (flag & UDP_FLAG_BROADCAST) {
            enable_broadcast = 1;
            if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enable_broadcast, sizeof (enable_broadcast)) < 0) {
                ncb_report_debug_information(ncb, "failed to set broadcast mode.err=%d\n", errno);
            }
        }

        /* 获取本地地址 */
        getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen);

        /* 附加到 EPOLL */
        retval = ioatth(ncb, kPollMask_Read);
        if (retval < 0) {
            break;
        }

        /* 关注数据包 */
        ncb->ncb_read = &udp_rx;
        ncb->ncb_write = &udp_tx;
        return hld;
    } while (0);

    objdefr(hld);
    objclos(hld);
    return -1;
}

void udp_destroy(HUDPLINK lnk) {
    objclos(lnk);
}

int udp_write(HUDPLINK lnk, int cb, nis_sender_maker_t maker, void *par, const char* r_ipstr, uint16_t r_port) {
    ncb_t *ncb;
    struct sockaddr_in addr;
    objhld_t hld = (objhld_t) lnk;
    unsigned char *buffer;
    int retval, offset;

    if (!maker || !r_ipstr || (0 == r_port) || (cb <= 0) || (lnk < 0) || (cb > MTU)) {
        return -1;
    }

    ncb = (ncb_t *) objrefr(hld);
    if (!ncb) {
        return -1;
    }

    buffer = NULL;
    do {
        /* 发送队列过长， 或者当前处于发送IO隔离阶段， 无法进行发送操作 */
        if ((fque_size(&ncb->tx_fifo) >= UDP_MAXIMUM_SENDER_CACHED_CNT) || ncb_if_wblocked(ncb)) {
            break;
        }

        buffer = (unsigned char *) malloc(cb);
        if (!buffer) {
            break;
        }

        /* 自己构造包数据 */
        if ((*maker)(buffer, cb, par) < 0) {
            break;
        }

        /* 向发送队列增加一个节点, 并投递激活消息 */
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(r_ipstr);
        addr.sin_port = htons(r_port);
        
         /* 如果写IO阻塞， 则只能使用任务模式 */
        if (ncb_if_wblocked(ncb)) {
            fque_push(&ncb->tx_fifo, buffer, cb, &addr);
            post_write_task(hld, kTaskType_TxTest);
        }
        
        /* 因为UDP并不强保证包序，因此可以尝试直接发送数据, 且不用关注线程问题 */
        else {
            offset = 0;
            retval = udp_direct_tx(ncb, buffer, &offset, cb, &addr);
            if (retval < 0) {
                break; 
            }
            if (0 != retval) {
                if (EAGAIN != retval) {
                    break;
                }
                fque_priority_push(&ncb->tx_fifo, buffer, cb - offset, offset, &addr);
                post_write_task(hld, kTaskType_TxTest);
            }
        }
        objdefr(hld);
        return 0;
    } while (0);

    if (buffer) {
        free(buffer);
    }
    objdefr(hld);
    return -1;
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

    retval = getsockopt(ncb->sockfd, level, opt, val, (socklen_t *) & len);

    objdefr(hld);
    return retval;
}