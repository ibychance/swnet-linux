#include "tcp.h"

#include "mxx.h"
#include "fifo.h"
#include "io.h"
#include "wpool.h"
#include "pipe.h"

#include "posix_ifos.h"
#include "posix_atomic.h"

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
const char *TCP_KERNEL_STATE_NAME[TCP_KERNEL_STATE_LIST_SIZE] = {
    "TCP_UNDEFINED",
    "TCP_ESTABLISHED",
    "TCP_SYN_SENT",
    "TCP_SYN_RECV",
    "TCP_FIN_WAIT1",
    "TCP_FIN_WAIT2",
    "TCP_TIME_WAIT",
    "TCP_CLOSE",
    "TCP_CLOSE_WAIT",
    "TCP_LAST_ACK",
    "TCP_LISTEN",
    "TCP_CLOSING"
};

static
int __tcprefr( objhld_t hld, ncb_t **ncb )
{
    if ( hld < 0 || !ncb) {
        return -EINVAL;
    }

    *ncb = objrefr( hld );
    if ( NULL != (*ncb) ) {
        if ( (*ncb)->protocol == IPPROTO_TCP ) {
            return 0;
        }

        objdefr( hld );
        *ncb = NULL;
        return -EPROTOTYPE;
    }

    return -ENOENT;
}

void tcp_update_opts(const ncb_t *ncb)
{
    if (ncb) {
#if 0
        /* define in:
         /proc/sys/net/ipv4/tcp_me
         /proc/sys/net/ipv4/tcp_wmem
         /proc/sys/net/ipv4/tcp_rmem */
        ncb_set_window_size(ncb, SO_RCVBUF, 65536);
        ncb_set_window_size(ncb, SO_SNDBUF, 65536);
#endif
        /* atomic keepalive */
        tcp_set_keepalive(ncb, 1);
        tcp_set_keepalive_value(ncb, 30, 5, 6);

        /* disable the Nginx algorithm */
        tcp_set_nodelay(ncb, 1);
    }
}

/* tcp impls */
int tcp_init()
{
	int retval;

	retval = io_init(IPPROTO_TCP);
	if (0 != retval) {
		return retval;
	}

	retval = wp_init(IPPROTO_TCP);
    if (retval < 0) {
        io_uninit(IPPROTO_TCP);
    }

    return retval;
}

void tcp_uninit()
{
    ncb_uninit(IPPROTO_TCP);
    io_uninit(IPPROTO_TCP);
    wp_uninit(IPPROTO_TCP);
}

HTCPLINK tcp_create(tcp_io_callback_t callback, const char* ipstr, uint16_t port)
{
    int fd;
    struct sockaddr_in addrlocal;
    int retval;
    int optval;
    ncb_t *ncb;
    objhld_t hld;

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        nis_call_ecr("[nshost.tcp.create] fatal error occurred syscall socket(2),error:%d", errno);
        return -1;
    }

    /* allow port reuse */
    optval = 1;
    retval = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof ( optval));

    /* binding address, and then allocate NCB object */
    addrlocal.sin_addr.s_addr = ipstr ? inet_addr(ipstr) : INADDR_ANY;
    addrlocal.sin_family = AF_INET;
    addrlocal.sin_port = htons(port);
    retval = bind(fd, (struct sockaddr *) &addrlocal, sizeof ( struct sockaddr));
    if (retval < 0) {
        nis_call_ecr("[nshost.tcp.create] fatal error occurred syscall bind(2), local endpoint %s:%u, error:%d", (ipstr ? ipstr : "0.0.0.0"), port, errno);
        close(fd);
        return -1;
    }

    hld = objallo(sizeof(ncb_t), &ncb_allocator, &ncb_deconstruct, NULL, 0);
    if (hld < 0) {
        nis_call_ecr("[nshost.tcp.create] insufficient resource for allocate inner object.");
        close(fd);
        return -1;
    }
    ncb = objrefr(hld);
    assert(ncb);

    do {
        ncb->hld = hld;
        ncb->sockfd = fd;
        ncb->protocol = IPPROTO_TCP;
        ncb->nis_callback = callback;
        memcpy(&ncb->local_addr, &addrlocal, sizeof (addrlocal));

        /* acquire save TCP Info and adjust linger in the creation phase. */
        ncb_set_linger(ncb, 0, 0);

        /* allocate normal TCP package */
        if (NULL == (ncb->packet = (unsigned char *) malloc(TCP_BUFFER_SIZE))) {
            retval = -ENOMEM;
            break;
        }

        /* zeroization protocol head*/
        ncb->u.tcp.rx_parse_offset = 0;
        if (NULL == (ncb->u.tcp.rx_buffer = (unsigned char *) malloc(TCP_BUFFER_SIZE))) {
            retval = -ENOMEM;
            break;
        }

        objdefr(hld);
        return hld;
    } while (0);

    objdefr(hld);
    objclos(hld);
    return retval;
}

int tcp_settst(HTCPLINK link, const tst_t *tst)
{
    ncb_t *ncb;
    int retval;

    if (!tst) {
        return -EINVAL;
    }

     /* size of tcp template must be less or equal to 32 bytes */
    if (tst->cb_ > TCP_MAXIMUM_TEMPLATE_SIZE) {
        nis_call_ecr("[nshost.tcp.settst] tst size must less than 32 byte.");
        return -EINVAL;
    }

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    ncb->u.tcp.template.cb_ = tst->cb_;
    ncb->u.tcp.template.builder_ = tst->builder_;
    ncb->u.tcp.template.parser_ = tst->parser_;
    objdefr(link);
    return retval;
}

int tcp_settst_r(HTCPLINK link, const tst_t *tst)
{
    ncb_t *ncb;
    int retval;

    if (!tst) {
        return -EINVAL;
    }

     /* size of tcp template must be less or equal to 32 bytes */
    if (tst->cb_ > TCP_MAXIMUM_TEMPLATE_SIZE) {
        nis_call_ecr("[nshost.tcp.settst] tst size must less than 32 byte.");
        return -EINVAL;
    }

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    ncb->u.tcp.prtemplate.cb_ = __sync_lock_test_and_set(&ncb->u.tcp.template.cb_, tst->cb_);
    ncb->u.tcp.prtemplate.builder_ = __sync_lock_test_and_set(&ncb->u.tcp.template.builder_, tst->builder_);
    ncb->u.tcp.prtemplate.parser_ = __sync_lock_test_and_set(&ncb->u.tcp.template.parser_, tst->parser_);
    objdefr(link);
    return retval;
}

int tcp_gettst(HTCPLINK link, tst_t *tst)
{
    ncb_t *ncb;
    int retval;

    if (!tst) {
        return -EINVAL;
    }

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    tst->cb_ = ncb->u.tcp.template.cb_;
    tst->builder_ = ncb->u.tcp.template.builder_;
    tst->parser_ = ncb->u.tcp.template.parser_;
    objdefr(link);
    return retval;
}

int tcp_gettst_r(HTCPLINK link, tst_t *tst, tst_t *previous)
{
    ncb_t *ncb;
    int retval;
    tst_t local;

    if (!tst) {
        return -EINVAL;
    }

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    local.cb_ = __sync_lock_test_and_set(&tst->cb_, ncb->u.tcp.template.cb_);
    local.builder_ = __sync_lock_test_and_set(&tst->builder_, ncb->u.tcp.template.builder_);
    local.parser_ = __sync_lock_test_and_set(&tst->parser_, ncb->u.tcp.template.parser_);
    objdefr(link);

    if (previous) {
        memcpy(previous, &local, sizeof(local));
    }
    return retval;
}

/*
 * Object destruction operations may be intended to interrupt some blocking operations. just like @tcp_connect
 * so,close the file descriptor directly, destroy the object by the smart pointer.
 */
void tcp_destroy(HTCPLINK link)
{
    ncb_t *ncb;

    /* it should be the last reference operation of this object, no matter how many ref-count now. */
    ncb = objreff(link);
    if (ncb) {
        nis_call_ecr("[nshost.tcp.destroy] link:%lld order to destroy", ncb->hld);
        io_close(ncb);
        objdefr(link);
    }
}

#if 0

/* <tcp_check_connection_bypoll> */
static int __tcp_check_connection_bypoll(int sockfd)
{
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
static int __tcp_check_connection(int sockfd)
{
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

int tcp_connect(HTCPLINK link, const char* ipstr, uint16_t port)
{
    ncb_t *ncb;
    int retval;
    struct sockaddr_in addr_to;
    socklen_t addrlen;
    int optval;
    struct tcp_info ktcp;

    if (link < 0 || !ipstr || 0 == port ) {
        return -EINVAL;
    }

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    do {
        retval = -1;

        /* get the socket status of tcp_info to check the socket tcp statues */
        if (tcp_save_info(ncb, &ktcp) >= 0) {
            if (ktcp.tcpi_state != TCP_CLOSE) {
                nis_call_ecr("[nshost.tcp.connect] state illegal,link:%lld, kernel states:%s.", link, tcp_state2name(ktcp.tcpi_state));
                break;
            }
        }

        /* try no more than 3 times of tcp::syn */
        optval = 3;
        setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_SYNCNT, &optval, sizeof (optval));

        /* set other options */
        /* On individual connections, the socket buffer size must be set prior to the listen(2) or connect(2) calls in order to have it take effect. */
        tcp_update_opts(ncb);

        addr_to.sin_family = PF_INET;
        addr_to.sin_port = htons(port);
        addr_to.sin_addr.s_addr = inet_addr(ipstr);

        /* syscall @connect can be interrupted by other signal. */
        do {
            retval = connect(ncb->sockfd, (const struct sockaddr *) &addr_to, sizeof (struct sockaddr));
        } while((errno == EINTR) && (retval < 0));

        if (retval < 0) {
            /* if this socket is already connected, or it is in listening states, sys-call failed with error EISCONN  */
            nis_call_ecr("[nshost.tcp.connect] fatal error occurred syscall connect(2), %s:%u, error:%u, link:%lld", ipstr, port, errno, link);
            break;
        }

        /*
         * set file descriptor in asynchronous mode,
         * and than, queue object into epoll manager
         *
         * on success, MUST attach this file descriptor to epoll as early as possible.
         * Even so, It is also possible a close message post to calling thread early then connected message  */
        retval = io_attach(ncb, EPOLLIN);
        if (retval < 0) {
            objclos(link);
            break;
        }

        /* save address information after connect successful */
        addrlen = sizeof (addr_to);
        getpeername(ncb->sockfd, (struct sockaddr *) &ncb->remot_addr, &addrlen); /* remote address information */
        getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen); /* local address information */

        /* render the connected event to up-level */
        ncb_post_connected(ncb);

        /* set handler function pointer to Rx/Tx */
        posix__atomic_set(&ncb->ncb_read, &tcp_rx);
        posix__atomic_set(&ncb->ncb_write, &tcp_tx);

    }while( 0 );

    objdefr(link);
    return retval;
}

int tcp_connect2(HTCPLINK link, const char* ipstr, uint16_t port)
{
    ncb_t *ncb;
    int retval;
    int optval;
    struct tcp_info ktcp;

    if (!ipstr || 0 == port || link < 0 ) {
        return -EINVAL;
    }

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    do {
        retval = -1;

        /* for asynchronous connect, set file-descriptor to non-blocked mode first */
        if (io_fcntl(ncb->sockfd) < 0) {
            break;
        }

        /* get the socket status of tcp_info to check the socket tcp statues */
        if (tcp_save_info(ncb, &ktcp) >= 0) {
            if (ktcp.tcpi_state != TCP_CLOSE) {
                nis_call_ecr("[nshost.tcp.connect2] state illegal,link:%lld, kernel states:%s.", link, tcp_state2name(ktcp.tcpi_state));
                break;
            }
        }

        /* double check the tx_syn routine */
        if ((NULL != posix__atomic_compare_ptr_xchange(&ncb->ncb_write, NULL, &tcp_tx_syn))) {
            nis_call_ecr("[nshost.tcp.connect2] link:%lld multithreading double call is not allowed.", link);
            break;
        }

        /* try no more than 3 times for tcp::syn */
        optval = 3;
        setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_SYNCNT, &optval, sizeof (optval));

        ncb->remot_addr.sin_family = PF_INET;
        ncb->remot_addr.sin_port = htons(port);
        ncb->remot_addr.sin_addr.s_addr = inet_addr(ipstr);

        do {
            retval = connect(ncb->sockfd, (const struct sockaddr *) &ncb->remot_addr, sizeof (struct sockaddr));
        }while((EINTR == errno) && (retval < 0));

        /* immediate success, some BSD/SystemV maybe happen */
        if ( 0 == retval) {
            nis_call_ecr("[nshost.tcp.connect2] asynchronous file descriptor but success immediate, link:%lld", link);
            tcp_tx_syn(ncb);
            break;
        }

        /*
         *  queue object to epoll manage befor syscall @connect,
         *  epoll_wait will get a EPOLLOUT signal when syn success.
         *  so, file descriptor MUST certain be in asynchronous mode before next stage
         *
         *  attach MUST early than connect(2) call,
         *  in some case, very short time after connect(2) called, the EPOLLRDHUP event has been arrived,
         *  if attach not in time, error information maybe lost, then bring the file-descriptor leak.
         *
         *  ncb object willbe destroy on fatal.
         *
         *  EPOLLOUT adn EPOLLHUP for asynchronous connect(2):
         *  1.When the connect function is not called locally, but the socket is attach to epoll for detection,
         *       epoll will generate an EPOLLOUT | EPOLLHUP, that is, an event with a value of 0x14
         *   2.When the local connect event occurs, but the connection fails to be established,
         *       epoll will generate EPOLLIN | EPOLLERR | EPOLLHUP, that is, an event with a value of 0x19
         *   3.When the connect function is also called and the connection is successfully established,
         *       epoll will generate EPOLLOUT once, with a value of 0x4, indicating that the socket is writable
        */
        if (EINPROGRESS == errno ) {
            retval = io_attach(ncb, EPOLLOUT);
            if ( retval < 0) {
                objclos(link);
            }
            break;
        }

        if (EAGAIN == errno) {
            nis_call_ecr("[nshost.tcp.connect2] Insufficient entries in the routing cache, link:%lld", link);
        } else {
            nis_call_ecr("[nshost.tcp.connect2] fatal error occurred syscall connect(2) to target endpoint %s:%u, error:%d, link:%lld", ipstr, port, errno, link);
        }

    } while (0);

    objdefr(link);
    return retval;
}

int tcp_listen(HTCPLINK link, int block)
{
    ncb_t *ncb;
    int retval;
    struct tcp_info ktcp;
    socklen_t addrlen;

    if (block < 0 || block >= 0x7FFF) {
        return -EINVAL;
    }

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    do {
        retval = -1;

        /* get the socket status of tcp_info to check the socket tcp statues */
        if (tcp_save_info(ncb, &ktcp) >= 0) {
            if (ktcp.tcpi_state != TCP_CLOSE) {
                nis_call_ecr("[nshost.tcp.listen] state illegal,link:%lld, kernel states:%s.", link, tcp_state2name(ktcp.tcpi_state));
                break;
            }
        }

        /*
         * '/proc/sys/net/core/somaxconn' in POSIX.1 this value default to 128
         *  so,for ensure high concurrency performance in the establishment phase of the TCP connection,
         *  we will ignore the @block argument and use macro SOMAXCONN which defined in /usr/include/bits/socket.h anyway */
        retval = listen(ncb->sockfd, ((0 == block) || (block > SOMAXCONN)) ? SOMAXCONN : block);
        if (retval < 0) {
            nis_call_ecr("[nshost.tcp.listen] fatal error occurred syscall listen(2),error:%u", errno);
            break;
        }

        /* this NCB object is readonly， and it must be used for accept */
        if (NULL != posix__atomic_compare_ptr_xchange(&ncb->ncb_read, NULL, &tcp_syn)) {
            nis_call_ecr("[nshost.tcp.tcp_listen] multithreading double call is not allowed,link:%lld", link);
            retval = -1;
            break;
        }
        posix__atomic_set(&ncb->ncb_write, NULL);

        /* set file descriptor to asynchronous mode and attach to it's own epoll object,
         *  ncb object willbe destroy on fatal. */
        if (io_attach(ncb, EPOLLIN) < 0) {
            objclos(link);
            break;
        }

        /*
         * allow application to listen on the random port,
         * therefor, framework MUST query the real address information for this file descriptor now */
        addrlen = sizeof(struct sockaddr);
        getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen);

        nis_call_ecr("[nshost.tcp.listen] success listen on link:%lld", link);
        retval = 0;
    } while (0);

    objdefr(link);
    return retval;
}

int tcp_awaken(HTCPLINK link, const void *pipedata, int cb)
{
    int retval;
    ncb_t *ncb;

    if (link < 0) {
        return -EINVAL;
    }

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    retval = pipe_write_message(ncb, pipedata, cb);

    objdefr(link);
    return retval;
}

int tcp_write(HTCPLINK link, const void *origin, int cb, const nis_serializer_t serializer)
{
    ncb_t *ncb;
    unsigned char *buffer;
    int packet_length;
    struct tcp_info ktcp;
    struct tx_node *node;
    int retval;

    if ( link < 0 || cb <= 0 || cb > TCP_MAXIMUM_PACKET_SIZE || !origin) {
        return -EINVAL;
    }

    buffer = NULL;
    node = NULL;

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    do {
        retval = -1;

        /* the following situation maybe occur when tcp_write called:
         * immediately call @tcp_write after @tcp_create, but no connection established and no listening has yet been taken
         * in this situation, @wpool::run_task maybe take a task, but @ncb->ncb_write is ineffectiveness.application may crashed.
         * examine these two parameters to ensure their effectiveness
         */
        if (!ncb->ncb_write || !ncb->ncb_read) {
            retval = -EINVAL;
            break;
        }

        /* get the socket status of tcp_info to check the socket tcp statues */
        if (tcp_save_info(ncb, &ktcp) >= 0) {
            if (ktcp.tcpi_state != TCP_ESTABLISHED) {
                nis_call_ecr("[nshost.tcp.write] state illegal,link:%lld, kernel states:%s.", link, tcp_state2name(ktcp.tcpi_state));
                break;
            }
        }

        /* if @template.builder is not null then use it, otherwise,
            indicate that calling thread want to specify the packet length through input parameter @cb */
        if (!(*ncb->u.tcp.template.builder_) || (ncb->attr & LINKATTR_TCP_NO_BUILD)) {
            packet_length = cb;
            if (NULL == (buffer = (unsigned char *) malloc(packet_length))) {
                retval = -ENOMEM;
                break;
            }

            /* serialize data into packet or direct use data pointer by @origin */
            if (serializer) {
                if ((*serializer)(buffer, origin, cb) < 0 ) {
                    nis_call_ecr("[nshost.tcp.write] fatal usrcall serializer.");
                    break;
                }
            } else {
                memcpy(buffer, origin, cb);
            }

        } else {
            packet_length = cb + ncb->u.tcp.template.cb_;
            if (NULL == (buffer = (unsigned char *) malloc(packet_length))) {
                retval = -ENOMEM;
                break;
            }

            /* build protocol head */
            if ((*ncb->u.tcp.template.builder_)(buffer, cb) < 0) {
                nis_call_ecr("[nshost.tcp.write] fatal usrcall tst.builder");
                break;
            }

            /* serialize data into packet or direct use data pointer by @origin */
            if (serializer) {
                if ((*serializer)(buffer + ncb->u.tcp.template.cb_, origin, cb) < 0 ) {
                    nis_call_ecr("[nshost.tcp.write] fatal usrcall serializer.");
                    break;
                }
            } else {
                memcpy(buffer + ncb->u.tcp.template.cb_, origin, cb );
            }
        }

        if (NULL == (node = (struct tx_node *) malloc(sizeof (struct tx_node)))) {
            retval = -ENOMEM;
            break;
        }
        memset(node, 0, sizeof(struct tx_node));
        node->data = buffer;
        node->wcb = cb + ncb->u.tcp.template.cb_;
        node->offset = 0;

        if (!fifo_is_blocking(ncb)) {
            retval = tcp_txn(ncb, node);

            /*
             * the return value means direct failed when it equal to -1 or success when it greater than zero.
             * in these case, destroy memory resource outside loop, no matter what the actually result it is.
             */
            if (-EAGAIN != retval) {
                break;
            }
        }

        /*
         * 1. when the IO state is blocking, any send or write call certain to be fail immediately,
         *
         * 2. the meaning of -EAGAIN return by @tcp_txn is send or write operation cannot be complete immediately,
         *      IO state should change to blocking now
         *
         * one way to handle the above two aspects, queue data to the tail of fifo manager, preserve the sequence of output order
         * in this case, memory of @buffer and @node cannot be destroy until asynchronous completed
         *
         * after @fifo_queue success called, IO blocking flag is set, and EPOLLOUT event has been associated with ncb object.
         * wpool thread canbe awaken by any kernel cache writable event trigger
         *
         * meaning of return value by function call:
         *  -EINVAL: input parameter is invalidate
         *  -EBUSY:fifo cache is full for insert
         *  >0 : the actual size after @node has been queued
         *   0: impossible, in theory
         *
         * on failure of function call, @node and it's owned buffer MUST be free
         */
        retval = fifo_queue(ncb, node);
        if (retval < 0) {
            break;
        }

        objdefr(link);
        return 0;
    } while (0);

    if (buffer) {
        free(buffer);
    }

    if (node) {
        free(node);
    }

    objdefr(link);
    return retval;
}

int tcp_getaddr(HTCPLINK link, int type, uint32_t* ipv4, uint16_t* port)
{
    ncb_t *ncb;
    int retval;
    struct sockaddr_in *addr;

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    addr = (LINK_ADDR_LOCAL == type) ? &ncb->local_addr :
            ((LINK_ADDR_REMOTE == type) ? &ncb->remot_addr : NULL);

    if (addr) {
        if (ipv4) {
            *ipv4 = htonl(addr->sin_addr.s_addr);
        }
        if (port) {
            *port = htons(addr->sin_port);
        }
    } else {
        retval = -EINVAL;
    }

    objdefr(link);
    return retval;
}

int tcp_setopt(HTCPLINK link, int level, int opt, const char *val, int len)
{
    ncb_t *ncb;
    int retval;

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    retval = setsockopt(ncb->sockfd, level, opt, (const void *) val, (socklen_t) len);
    if (retval < 0) {
        nis_call_ecr("[nshost.tcp.tcp_setopt] fatal error occurred syscall setsockopt(2) with level:%d optname:%d,error:%d", level, opt, errno);
    }

    objdefr(link);
    return retval;
}

int tcp_getopt(HTCPLINK link, int level, int opt, char *__restrict val, int *len)
{
    ncb_t *ncb;
    int retval;

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    retval = getsockopt(ncb->sockfd, level, opt, (void * __restrict)val, (socklen_t *) len);
    if (retval < 0) {
        nis_call_ecr("[nshost.tcp.tcp_setopt] fatal error occurred syscall getsockopt(2) with level:%d optname:%d,error:%d", level, opt, errno);
    }

    objdefr(link);
    return retval;
}

int tcp_save_info(const ncb_t *ncb, struct tcp_info *ktcp)
{
    socklen_t len;

    if (!ncb || !ktcp) {
        return -EINVAL;
    }

    len = sizeof (struct tcp_info);
    return getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_INFO, (void * __restrict)ktcp, &len);
}

int tcp_setmss(const ncb_t *ncb, int mss)
{
    return (ncb && mss > 0) ?
            setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_MAXSEG, (const void *) &mss, sizeof (mss)) : -EINVAL;
}

int tcp_getmss(const ncb_t *ncb)
{
    socklen_t lenmss;
    if (ncb) {
        lenmss = sizeof (ncb->u.tcp.mss);
        return getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_MAXSEG, (void *__restrict) & ncb->u.tcp.mss, &lenmss);
    }
    return -EINVAL;
}

int tcp_set_nodelay(const ncb_t *ncb, int set)
{
    return ncb ? setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_NODELAY, (const void *) &set, sizeof ( set)) : -EINVAL;
}

int tcp_get_nodelay(const ncb_t *ncb, int *set)
{
    socklen_t optlen;

    if (ncb && set) {
        optlen = sizeof (int);
        return getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_NODELAY, (void *__restrict)set, &optlen);
    }
    return -EINVAL;
}

int tcp_set_cork(const ncb_t *ncb, int set)
{
    return ncb ? setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_CORK, (const void *) &set, sizeof ( set)) : -EINVAL;
}

int tcp_get_cork(const ncb_t *ncb, int *set)
{
    socklen_t optlen;

    if (ncb && set) {
        optlen = sizeof (int);
        return getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_CORK, (void *__restrict)set, &optlen);
    }
    return -EINVAL;
}

int tcp_set_keepalive(const ncb_t *ncb, int enable)
{
    return ncb ? setsockopt(ncb->sockfd, SOL_SOCKET, SO_KEEPALIVE, (const char *) &enable, sizeof ( enable)) : -EINVAL;
}

int tcp_get_keepalive(const ncb_t *ncb, int *enabled)
{
    socklen_t optlen;

    if (ncb && enabled) {
        optlen = sizeof(int);
        return getsockopt(ncb->sockfd, SOL_SOCKET, SO_KEEPALIVE, (void *__restrict)enabled, &optlen);
    }
    return -EINVAL;
}

int tcp_set_keepalive_value(const ncb_t *ncb, int idle, int interval, int probes)
{
    int enabled;
    if (tcp_get_keepalive(ncb, &enabled) < 0) {
        return -1;
    }

    if (!enabled) {
        return -1;
    }

    do {
        /* lanuch keepalive when no data transfer during @idle */
        if (setsockopt(ncb->sockfd, SOL_TCP, TCP_KEEPIDLE, (void *)&idle, sizeof(idle)) < 0) {
            break;
        }

        /* the interval of each keepalive check */
        if (setsockopt(ncb->sockfd, SOL_TCP, TCP_KEEPINTVL, (void *)&interval, sizeof(interval)) < 0) {
            break;
        }

        /* times of allowable keepalive failures */
        if (setsockopt(ncb->sockfd, SOL_TCP, TCP_KEEPCNT, (void *)&probes, sizeof(probes)) < 0) {
            break;
        }

        return 0;
    }while( 0 );

    return -1;
}

int tcp_get_keepalive_value(const ncb_t *ncb,int *idle, int *interval, int *probes)
{
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

int tcp_setattr(HTCPLINK link, int attr, int enable)
{
    ncb_t *ncb;
    int retval;

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    switch(attr) {
        case LINKATTR_TCP_FULLY_RECEIVE:
        case LINKATTR_TCP_NO_BUILD:
        case LINKATTR_TCP_UPDATE_ACCEPT_CONTEXT:
            (enable > 0) ? (ncb->attr |= attr) : (ncb->attr &= ~attr);
            retval = 0;
            break;
        default:
            retval = -EINVAL;
            break;
    }

    objdefr(link);
    return retval;
}

int tcp_getattr(HTCPLINK link, int attr, int *enabled)
{
    ncb_t *ncb;
    int retval;

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    if (ncb->attr & attr) {
        *enabled = 1;
    } else {
        *enabled = 0;
    }

    objdefr(link);
    return retval;
}

void tcp_setattr_r(ncb_t *ncb, int attr)
{
    __sync_lock_test_and_set(&ncb->attr, attr);
}

int tcp_getattr_r(ncb_t *ncb, int *attr)
{
    return __sync_lock_test_and_set(attr, ncb->attr);
}
