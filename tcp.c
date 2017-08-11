#include <stdio.h>
#include <time.h>
#include <sys/select.h>
#include <sys/time.h>

#include "tcp.h"

int tcp_update_opts(ncb_t *ncb) {
    if (!ncb) {
        return -1;
    }

    ncb_set_window_size(ncb, SO_RCVBUF, TCP_BUFFER_SIZE);
    ncb_set_window_size(ncb, SO_SNDBUF, TCP_BUFFER_SIZE);
    ncb_set_linger(ncb, 0, 1);
    ncb_set_keepalive(ncb, 1);

    tcp_set_nodelay(ncb, 1); /* Ϊ��֤С��Ч�ʣ� ���� Nginx �㷨 */
    tcp_save_info(ncb);

    return 0;
}

/* tcp impls */
int tcp_init() {
    ioinit();
    write_pool_init();
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

    if (!user_callback) return INVALID_HTCPLINK;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("[TCP] failed create services socket.errno=%d.\n", errno);
        return -1;
    }

    /* ����˿ڸ��� */
    optval = 1;
    retval = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof ( optval));

    /* �󶨵�ַ���������� */
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
        ncb->hld = hld;
        ncb->sockfd = fd;
        ncb->proto_type = kProtocolType_TCP;
        ncb->nis_callback = user_callback;
        memcpy(&ncb->local_addr, &addrlocal, sizeof (addrlocal));

        /*ETģ�ͱ��뱣�������ļ��������첽����*/
        if (setasio(ncb->sockfd) < 0) {
            break;
        }

        /* setsockopt */
        if (tcp_update_opts(ncb) < 0) {
            break;
        }

        /*����TCP��ͨ��*/
        ncb->packet = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb->packet) {
            break;
        }

        /*���Э��ͷ*/
        ncb->rx_parse_offset = 0;
        ncb->rx_buffer = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb->rx_buffer) {
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

    ncb->template.cb_ = tst->cb_;
    ncb->template.builder_ = tst->builder_;
    ncb->template.parser_ = tst->parser_;
    objdefr((objhld_t) lnk);
    return 0;
}

int tcp_gettst(HTCPLINK lnk, tst_t *tst) {
    ncb_t *ncb;

    if (lnk < 0 || !tst) return -1;

    ncb = (ncb_t *) objrefr((objhld_t) lnk);
    if (!ncb) return -1;

    tst->cb_ = ncb->template.cb_;
    tst->builder_ = ncb->template.builder_;
    tst->parser_ = ncb->template.parser_;
    return 0;
}

/*
 * �ر���Ӧ���:
 * �������ٲ����п�����ϣ���ж�ĳЩ���������� �� connect
 * �ʽ�������Ϊ����Ϊֱ�ӹر��������� ͨ������ָ�����ٶ���
 */
void tcp_destroy(HTCPLINK lnk) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;

    ncb = objrefr(hld);
    if (ncb) {
        ioclose(ncb);
        objdefr(hld);
    }

    objclos(hld);
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
    int retval;
    socklen_t len;
    struct timeval timeo;
    fd_set set;
    int error;

    // 3m ��Ϊ���ʱ
    timeo.tv_sec = 3;
    timeo.tv_usec = 0;

    FD_ZERO(&set);
    FD_SET(sockfd, &set);

    retval = -1;
    len = sizeof (error);
    do {
        /*
         * The nfds argument specifies the range of descriptors to be tested. 
         * The first nfds descriptors shall be checked in each set; 
         * that is, the descriptors from zero through nfds-1 in the descriptor sets shall be examined. 
         * */
        if (select(sockfd + 1, NULL, &set, NULL, &timeo) <= 0) {
            break;
        }

        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, (socklen_t *) & len) < 0) {
            break;
        }

        retval = error;
    } while (0);

    return retval;
}

int tcp_connect(HTCPLINK lnk, const char* r_ipstr, uint16_t port_remote) {
    ncb_t *ncb;
    int retval;
    struct sockaddr_in addr_to;
    int e;
    socklen_t addrlen;
    int optval;
    nis_event_t c_event;
    tcp_data_t c_data;

    if (lnk < 0 || !r_ipstr || 0 == port_remote) {
        return -1;
    }

    ncb = (ncb_t *) objrefr((objhld_t) lnk);
    if (!ncb) {
        return -1;
    }

    optval = 3;
    /* ��ȷ������ೢ��3�� syn ���� */
    setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_SYNCNT, &optval, sizeof (optval));

    addr_to.sin_family = PF_INET;
    addr_to.sin_port = htons(port_remote);
    addr_to.sin_addr.s_addr = inet_addr(r_ipstr);
    retval = connect(ncb->sockfd, (const struct sockaddr *) &addr_to, sizeof (struct sockaddr));
    e = errno;
    if (retval < 0) {
        if (e == EINPROGRESS) {
            /* �첽SOCKET������SYN�׶η��� EINPROGRESS ���󣬴�ʱ����Ӧ���Ѿ������� ������Ҫ��� */
            retval = tcp_check_connection(ncb->sockfd);
        }
    }

    if (retval >= 0) {
        /* �ɹ����Ӻ���Ҫȷ�����غͶԶ˵ĵ�ַ��Ϣ */
        addrlen = sizeof (addr_to);
        getpeername(ncb->sockfd, (struct sockaddr *) &ncb->remot_addr, &addrlen); /* �Զ˵ĵ�ַ��Ϣ */
        getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen); /* ���صĵ�ַ��Ϣ */

        /* ��ע�շ��� */
        ncb->ncb_read = &tcp_rx;
        ncb->ncb_write = &tcp_tx;

        retval = ioatth(ncb, kPollMask_Read);
        if (retval >= 0) {
            c_event.Event = EVT_TCP_CONNECTED;
            c_event.Ln.Tcp.Link = lnk;
            c_data.e.LinkOption.OptionLink = lnk;
            ncb->nis_callback(&c_event, &c_data);
        }
    } else {
        ncb_report_debug_information(ncb, "[TCP]failed connect remote endpoint %s:%d, err=%d\n", r_ipstr, port_remote, e);
    }

    objdefr((objhld_t) lnk);
    return retval;
}

int tcp_connect2(HTCPLINK lnk, const char* r_ipstr, uint16_t port_remote) {
    ncb_t *ncb;
    int retval;
    int e;
    int optval;

    if (lnk < 0 || !r_ipstr || 0 == port_remote) {
        return -1;
    }

    ncb = (ncb_t *) objrefr((objhld_t) lnk);
    if (!ncb) {
        return -1;
    }

    optval = 3;

    /* ��ȷ������ೢ��3�� syn ���� */
    setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_SYNCNT, &optval, sizeof (optval));

    retval = -1;
    do {
        /* �첽���ӣ� �� connect ǰ�� �Ȱ��׽��ֶ�����뵽 EPOLL ���У� һ�����Ϻ��õ�һ�� EPOLLOUT */
        ncb->ncb_write = &tcp_tx_syn;
        if (ioatth(ncb, kPollMask_Write) < 0) {
            break;
        }

        ncb->remot_addr.sin_family = PF_INET;
        ncb->remot_addr.sin_port = htons(port_remote);
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

    ncb = (ncb_t *) objrefr(lnk);
    if (!ncb) {
        return -1;
    }

    retval = -1;
    do {
        retval = listen(ncb->sockfd, ((block > 0) ? (block) : (SOMAXCONN)));
        if (retval < 0) {
            ncb_report_debug_information(ncb, "failed syscall listen.errno=%d.\n", errno);
            break;
        }

        if (ioatth(ncb, kPollMask_Read) < 0) {
            break;
        }

        /* �� NCB ����ֻ���ܶ��������ݣ� ����һ���ǽ������� */
        ncb->ncb_read = &tcp_syn;
        ncb->ncb_write = NULL;

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
        /* ���Ͷ��й����� �޷����з��Ͳ��� */
        if (fque_size(&ncb->tx_fifo) >= TCP_MAXIMUM_SENDER_CACHED_CNT) {
            break;
        }

        /*�����кϷ�TSTָ��*/
        if (!(*ncb->template.builder_)) {
            ncb_report_debug_information(ncb, "[TCP]invalidated link object TST builder function address.\n");
            break;
        }

        /* �������ݻ����� */
        buffer = (unsigned char *) malloc(cb + ncb->template.cb_);
        if (!buffer) {
            break;
        }

        /*����Э��ͷ*/
        if ((*ncb->template.builder_)(buffer, cb) < 0) {
            break;
        }

        /*�û���������*/
        if ((*maker)(buffer + ncb->template.cb_, cb, par) < 0) {
            break;
        }

        /* ���Ͷ�������һ���ڵ�, ��Ͷ�ݼ�����Ϣ */
        if (fque_push(&ncb->tx_fifo, buffer, cb + ncb->template.cb_, NULL) < 0) {
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
    if (!ncb) return -1;

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
    if (!ncb) return -1;

    retval = setsockopt(ncb->sockfd, level, opt, (const void *) val, (socklen_t) len);

    objdefr(hld);
    return retval;
}

int tcp_getopt(HTCPLINK lnk, int level, int opt, char *__restrict val, int *len) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int retval;

    ncb = objrefr(hld);
    if (!ncb) return -1;

    retval = getsockopt(ncb->sockfd, level, opt, (void * __restrict)val, (socklen_t *) len);

    objdefr(hld);
    return retval;
}

int tcp_save_info(ncb_t *ncb) {
    socklen_t tcp_info_length = sizeof (struct tcp_info);

    if (!ncb->ktcp) {
        ncb->ktcp = (struct tcp_info *) malloc(sizeof (struct tcp_info));
    }

    if (ncb->ktcp) {
        return getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_INFO, (void * __restrict)ncb->ktcp, &tcp_info_length);
    }
    return -1;
}

int tcp_setmss(ncb_t *ncb, int mss) {
    if (ncb && mss > 0) {
        return setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_MAXSEG, (const void *) &mss, sizeof (mss));
    }

    return -EINVAL;
}

int tcp_getmss(ncb_t *ncb) {
    if (ncb) {
        socklen_t lenmss = sizeof (ncb->mss);
        return getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_MAXSEG, (void *__restrict) & ncb->mss, &lenmss);
    }
    return -EINVAL;
}

int tcp_set_nodelay(ncb_t *ncb, int set) {
    if (ncb) {
        return setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_NODELAY, (const void *) &set, sizeof ( set));
    }

    return -EINVAL;
}

int tcp_get_nodelay(ncb_t *ncb, int *set) {
    if (ncb && set) {
        socklen_t optlen = sizeof (int);
        return getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_NODELAY, (void *__restrict)set, &optlen);
    }
    return -EINVAL;
}