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

int tcp_allocate_rx_buffer(ncb_t *ncb)
{
    assert(ncb);

    /* allocate normal TCP package */
    if (!ncb->packet) {
        if (NULL == (ncb->packet = (unsigned char *) malloc(TCP_BUFFER_SIZE))) {
            mxx_call_ecr("fails allocate virtual memory for ncb->packet");
            return -ENOMEM;
        }
    }

    /* zeroization protocol head*/
    ncb->u.tcp.rx_parse_offset = 0;
    if (!ncb->u.tcp.rx_buffer) {
        if (NULL == (ncb->u.tcp.rx_buffer = (unsigned char *) malloc(TCP_BUFFER_SIZE))) {
            mxx_call_ecr("fails allocate virtual memory for ncb->u.tcp.rx_buffer");
            free(ncb->packet);
            ncb->packet = NULL;
            return -ENOMEM;
        }
    }
    return 0;
}

static int tcp_bind(const ncb_t *ncb)
{
    assert(ncb);

    /* the user specified a explicit address or port to bind when invoke @tcp_create */
    if (0 != ncb->local_addr.sin_addr.s_addr || 0 != ncb->local_addr.sin_port) {
        /* binding on local address before listen */
        if ( bind(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, sizeof(struct sockaddr)) < 0 ) {
            mxx_call_ecr("fatal syscall bind(2), errno:%d, link:%lld", errno, ncb->hld);
            return posix__makeerror(errno);
        }
    }

    return 0;
}

#define TCP_IMPLS_INVOCATION(foo)  foo(IPPROTO_TCP)

/* tcp impls */
int tcp_init()
{
	int retval;

	retval = TCP_IMPLS_INVOCATION(io_init);
	if (0 != retval) {
		return retval;
	}

	retval = TCP_IMPLS_INVOCATION(wp_init);
    if (retval < 0) {
        TCP_IMPLS_INVOCATION(io_uninit);
    }

    return retval;
}

void tcp_uninit()
{
    TCP_IMPLS_INVOCATION(ncb_uninit);
    TCP_IMPLS_INVOCATION(io_uninit);
    TCP_IMPLS_INVOCATION(wp_uninit);
}

HTCPLINK tcp_create(tcp_io_callback_t callback, const char* ipstr, uint16_t port)
{
    int fd;
    ncb_t *ncb;
    objhld_t hld;

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        mxx_call_ecr("fatal error occurred syscall socket(2),error:%d", errno);
        return -1;
    }

    hld = objallo(sizeof(ncb_t), &ncb_allocator, &ncb_deconstruct, NULL, 0);
    if (hld < 0) {
        mxx_call_ecr("insufficient resource for allocate inner object.");
        close(fd);
        return -1;
    }
    ncb = objrefr(hld);
    assert(ncb);

    ncb->hld = hld;
    ncb->sockfd = fd;
    ncb->protocol = IPPROTO_TCP;
    ncb->nis_callback = callback;

    /* local address fill and delay use */
    ncb->local_addr.sin_addr.s_addr = ipstr ? inet_addr(ipstr) : INADDR_ANY;
    ncb->local_addr.sin_family = AF_INET;
    ncb->local_addr.sin_port = htons(port);

    /* every TCP socket do NOT need graceful close */
    ncb_set_linger(ncb);
    mxx_call_ecr("success allocate link:%lld, sockfd:%d", ncb->hld, ncb->sockfd);
    objdefr(hld);
    return hld;
}

HTCPLINK tcp_create2(tcp_io_callback_t callback, const char* ipstr, uint16_t port, const tst_t *tst)
{
    HTCPLINK link;

    link = tcp_create(callback, ipstr, port);
    if (INVALID_HTCPLINK == link) {
        return INVALID_HTCPLINK;
    }

    if (tst) {
        if (tcp_settst_r(link, tst) < 0) {
            tcp_destroy(link);
            link = INVALID_HTCPLINK;
        }
    }

    return link;
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
       mxx_call_ecr("tst size must less than 32 byte.");
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
        mxx_call_ecr("tst size must less than 32 byte.");
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
        mxx_call_ecr("link:%lld order to destroy", ncb->hld);
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
    struct tcp_info ktcp;

    if (link < 0 || !ipstr || 0 == port || 0xFFFF == port ) {
        return -EINVAL;
    }

    if (0 == ipstr[0]) {
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
                mxx_call_ecr("state illegal,link:%lld, kernel states:%s.", link, tcp_state2name(ktcp.tcpi_state));
                retval = ( TCP_ESTABLISHED == ktcp.tcpi_state ) ? -EISCONN : -EBADFD;
                break;
            }
        }

        /* set time elapse for TCP sender timeout error trigger */
        tcp_set_user_timeout(ncb, 3000);
        /* try no more than 3 times of tcp::syn */
        tcp_set_syncnt(ncb, 3);
        /* On individual connections, the socket buffer size must be set prior to the listen(2) or connect(2) calls in order to have it take effect. */
        ncb_set_buffsize(ncb);
        /* mark normal attributes */
        tcp_set_nodelay(ncb, 1);

        /* bind on particular local address:port tuple when need. */
        if ( (retval = tcp_bind(ncb)) < 0 ) {
            break;
        }

        addr_to.sin_family = PF_INET;
        addr_to.sin_port = htons(port);
        addr_to.sin_addr.s_addr = inet_addr(ipstr);

        /* syscall @connect can be interrupted by other signal. */
        do {
            retval = connect(ncb->sockfd, (const struct sockaddr *) &addr_to, sizeof (struct sockaddr));
        } while((errno == EINTR) && (retval < 0));

        if (retval < 0) {
            /* if this socket is already connected, or it is in listening states, sys-call failed with error EISCONN  */
            mxx_call_ecr("fatal error occurred syscall connect(2), %s:%u, error:%u, link:%lld", ipstr, port, errno, link);
            break;
        }

        /* this link use to receive data from remote peer,
            so the packet and rx memory acquire to allocate now */
        if ( tcp_allocate_rx_buffer(ncb) < 0 ) {
            objclos(link);
            break;
        }
        /* the low-level [TCP Keep-ALive] are usable. */
        tcp_set_keepalive(ncb);

        /* get peer address information */
        tcp_relate_address(ncb);

        /* follow tcp rx/tx event */
        posix__atomic_set(&ncb->ncb_read, &tcp_rx);
        posix__atomic_set(&ncb->ncb_write, &tcp_tx);

        /* focus EPOLLIN only */
        if ((retval = io_attach(ncb, EPOLLIN)) < 0) {
            objclos(link);
            break;
        }

        mxx_call_ecr("connection established associated binding on %s:%d, link:%lld .",
            inet_ntoa(ncb->local_addr.sin_addr), ntohs(ncb->local_addr.sin_port), ncb->hld);
        ncb_post_connected(ncb);
    }while( 0 );

    objdefr(link);
    return retval;
}

int tcp_connect2(HTCPLINK link, const char* ipstr, uint16_t port)
{
    ncb_t *ncb;
    int retval;
    struct tcp_info ktcp;

    if (!ipstr || 0 == port || link < 0 || 0xff == port ) {
        return -EINVAL;
    }

    if (0 == ipstr[0]) {
        return -EINVAL;
    }

    retval = __tcprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    do {
        retval = -1;

        /* for asynchronous connect, set file-descriptor to non-blocked mode first */
        if (io_fnbio(ncb->sockfd) < 0) {
            break;
        }

        /* get the socket status of tcp_info to check the socket tcp statues */
        if (tcp_save_info(ncb, &ktcp) >= 0) {
            if (ktcp.tcpi_state != TCP_CLOSE) {
                mxx_call_ecr("state illegal,link:%lld, kernel states:%s.", link, tcp_state2name(ktcp.tcpi_state));
                retval = ( TCP_ESTABLISHED == ktcp.tcpi_state ) ? -EISCONN : -EBADFD;
                break;
            }
        }

        /* set time elapse for TCP sender timeout error trigger */
        tcp_set_user_timeout(ncb, 3000);
        /* try no more than 3 times for tcp::syn */
        tcp_set_syncnt(ncb, 3);
        /* On individual connections, the socket buffer size must be set prior to the listen(2) or connect(2) calls in order to have it take effect. */
        ncb_set_buffsize(ncb);
        /* mark normal attributes */
        tcp_set_nodelay(ncb, 1);

        /* bind on particular local address:port tuple when need. */
        if ( (retval = tcp_bind(ncb)) < 0) {
            break;
        }

        /* double check the tx_syn routine */
        if ((NULL != __sync_val_compare_and_swap(&ncb->ncb_write, NULL, &tcp_tx_syn))) {
            mxx_call_ecr("link:%lld multithreading double call is not allowed.", link);
            break;
        }

        ncb->remot_addr.sin_family = PF_INET;
        ncb->remot_addr.sin_port = htons(port);
        ncb->remot_addr.sin_addr.s_addr = inet_addr(ipstr);

        do {
            retval = connect(ncb->sockfd, (const struct sockaddr *) &ncb->remot_addr, sizeof (struct sockaddr));
        }while((EINTR == errno) && (retval < 0));

        /* immediate success, some BSD/SystemV maybe happen */
        if ( 0 == retval) {
            mxx_call_ecr("asynchronous file descriptor but success immediate, link:%lld", link);
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
         *  EPOLLOUT and EPOLLHUP for asynchronous connect(2):
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
            mxx_call_ecr("Insufficient entries in the routing cache, link:%lld", link);
        } else {
            mxx_call_ecr("fatal error occurred syscall connect(2) to target endpoint %s:%u, error:%d, link:%lld", ipstr, port, errno, link);
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
                mxx_call_ecr("state illegal,link:%lld, kernel states:%s.", link, tcp_state2name(ktcp.tcpi_state));
                retval = EBADFD;
                break;
            }
        }

        /* allow port reuse(the same port number binding on different IP address) */
        ncb_set_reuseaddr(ncb);

        /* binding on local adpater before listen */
        if ( ( retval = tcp_bind(ncb)) < 0) {
            break;
        }

        /* On individual connections, the socket buffer size must be set prior to the listen(2) or connect(2) calls in order to have it take effect. */
#if 0
        ncb_set_buffsize(ncb);
#endif
        /*
         * '/proc/sys/net/core/somaxconn' in POSIX.1 this value default to 128
         *  so,for ensure high concurrency performance in the establishment phase of the TCP connection,
         *  we will ignore the @block argument and use macro SOMAXCONN which defined in /usr/include/bits/socket.h anyway */
        retval = listen(ncb->sockfd, ((0 == block) || (block > SOMAXCONN)) ? SOMAXCONN : block);
        if (retval < 0) {
            mxx_call_ecr("fatal error occurred syscall listen(2),error:%u", errno);
            retval = posix__makeerror(errno);
            break;
        }

        /* this NCB object is readonly， and it must be used for accept */
        if (NULL != posix__atomic_compare_ptr_xchange(&ncb->ncb_read, NULL, &tcp_syn)) {
            mxx_call_ecr("multithreading double call is not allowed,link:%lld", link);
            retval = -1;
            break;
        }
        posix__atomic_set(&ncb->ncb_write, NULL);

        /* set file descriptor to asynchronous mode and attach to it's own epoll object,
         *  ncb object willbe destroy on fatal. */
        if ( (retval = io_attach(ncb, EPOLLIN)) < 0) {
            objclos(link);
            break;
        }

        /*
         * allow application to listen on the random port,
         * therefor, framework MUST query the real address information for this file descriptor now */
        addrlen = sizeof(struct sockaddr);
        getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen);

        mxx_call_ecr("listen on %s:%d, link:%lld", inet_ntoa(ncb->local_addr.sin_addr), ntohs(ncb->local_addr.sin_port), link);
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
                mxx_call_ecr("state illegal,link:%lld, kernel states:%s.", link, tcp_state2name(ktcp.tcpi_state));
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
                    mxx_call_ecr("fatal usrcall serializer.");
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
                mxx_call_ecr("fatal usrcall tst.builder");
                break;
            }

            /* serialize data into packet or direct use data pointer by @origin */
            if (serializer) {
                if ((*serializer)(buffer + ncb->u.tcp.template.cb_, origin, cb) < 0 ) {
                    mxx_call_ecr("fatal usrcall serializer.");
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
        node->wcb = packet_length;
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

    objdefr(link);

    if (buffer) {
        free(buffer);
    }

    if (node) {
        free(node);
    }

    if (retval < 0) {
        /* the error code is device busy, disable this sent operation and allow other request pending.
            otherwise, close the session */
        if (-EBUSY != retval) {
            objclos(link);
        }
    }

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
        mxx_call_ecr("fatal error occurred syscall setsockopt(2) with level:%d optname:%d,error:%d", level, opt, errno);
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
        mxx_call_ecr("fatal error occurred syscall getsockopt(2) with level:%d optname:%d,error:%d", level, opt, errno);
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

int tcp_get_nodelay(const ncb_t *ncb)
{
    socklen_t optlen;
    int nodelay;

    optlen = sizeof (int);
    if ( getsockopt(ncb->sockfd, IPPROTO_TCP, TCP_NODELAY, (void *__restrict)&nodelay, &optlen) < 0 ) {
        return -1;
    }

    return nodelay;
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

int tcp_set_keepalive(const ncb_t *ncb)
{
    int optka;
    int optkintvl;
    int optkidle;
    int optkcnt;

    optka = 1;
    if ( setsockopt(ncb->sockfd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&optka, sizeof(optka)) < 0 ) {
        mxx_call_ecr("fatal syscall setsockopt(2) on level:%d, name:%d, errno:%d, link:%lld", SOL_SOCKET, SO_KEEPALIVE, errno, ncb->sockfd);
        return -1;
    }

    /* The time (in seconds) between individual keepalive probes */
    optkintvl = 2;
    if ( setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_KEEPINTVL, (const char *)&optkintvl, sizeof(optkintvl)) < 0 ) {
        mxx_call_ecr("fatal syscall setsockopt(2) on level:%d, name:%d, errno:%d, link:%lld", IPPROTO_TCP, TCP_KEEPINTVL, errno, ncb->sockfd);
        return -1;
    }

    /* The  time (in seconds) the connection needs to remain idle before TCP starts sending keepalive probes */
    optkidle = 4;
    if ( setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_KEEPIDLE, (const char *)&optkidle, sizeof(optkidle)) < 0 ) {
        mxx_call_ecr("fatal syscall setsockopt(2) on level:%d, name:%d, errno:%d, link:%lld", IPPROTO_TCP, TCP_KEEPIDLE, errno, ncb->sockfd);
        return -1;
    }

    /* The  time (in seconds) the connection needs to remain idle before TCP starts sending keepalive probes */
    optkcnt = 1;
    if ( setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_KEEPCNT, (const char *)&optkcnt, sizeof(optkcnt)) < 0 ) {
        mxx_call_ecr("fatal syscall setsockopt(2) on level:%d, name:%d, errno:%d, link:%lld", IPPROTO_TCP, TCP_KEEPIDLE, errno, ncb->sockfd);
        return -1;
    }

    return 0;
}

int tcp_set_syncnt(const ncb_t *ncb, int cnt)
{
    if ( setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_SYNCNT, (const void *)&cnt, sizeof(cnt)) < 0 ) {
        mxx_call_ecr("fatal syscall setsockopt(2) on level:%d, name:%d, errno:%d, link:%lld", IPPROTO_TCP, TCP_SYNCNT, errno, ncb->sockfd);
        return -1;
    }

    return 0;
}

int tcp_set_user_timeout(const ncb_t *ncb, unsigned int uto)
{
    if ( setsockopt(ncb->sockfd, IPPROTO_TCP, TCP_USER_TIMEOUT, (const char *)&uto, sizeof(uto)) < 0 ) {
        mxx_call_ecr("fatal syscall setsockopt(2) on level:%d, name:%d, errno:%d, link:%lld", IPPROTO_TCP, TCP_USER_TIMEOUT, errno, ncb->sockfd);
        return -1;
    }

    return 0;
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

void tcp_relate_address(ncb_t *ncb)
{
    socklen_t addrlen;

    assert(ncb);

    /* get peer address information */
    addrlen = sizeof (struct sockaddr);
    /* remote address information */
    if ( 0 != getpeername(ncb->sockfd, (struct sockaddr *) &ncb->remot_addr, &addrlen) ) {
        mxx_call_ecr("fatal error occurred syscall getpeername(2) with error:%d, on link:%lld", errno, link);
    }
    /* local address information */
    if (0 != getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen) ) {
        mxx_call_ecr("fatal error occurred syscall getsockname(2) with error:%d, on link:%lld", errno, link);
    }
}
