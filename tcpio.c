#include "tcp.h"

#include <poll.h>

#include "mxx.h"
#include "fifo.h"
#include "io.h"

#include "posix_atomic.h"

static
int __tcp_syn_try(ncb_t *ncb_server, int *clientfd, int *ctrlcode)
{
    struct sockaddr_in addr_income;
    socklen_t addrlen;

    if (!ncb_server || !clientfd || !ctrlcode) {
        return -EINVAL;
    }

    *ctrlcode = 1;

    addrlen = sizeof ( addr_income);
    *clientfd = accept(ncb_server->sockfd, (struct sockaddr *) &addr_income, &addrlen);
    if (*clientfd < 0) {
        switch (errno) {
        /* The system call was interrupted by a signal that was caught before a valid connection arrived, or this connection has been aborted.
            in these case , this round of operation ignore, try next round accept notified by epoll */
            case EINTR:
            case ECONNABORTED:
                *ctrlcode = 0;
                break;

             /* no more data canbe read, waitting for next epoll edge trigger */
            case EAGAIN:
                *ctrlcode = EAGAIN;
                break;

        /* The per-process/system-wide limit on the number of open file descriptors has been reached, or
            Not enough free memory, or Firewall rules forbid connection.
            in these cases, this round of operation can fail, but the service link must be retain */
            case ENFILE:
            case ENOBUFS:
            case ENOMEM:
            case EPERM:
                nis_call_ecr("[nshost.tcpio.__tcp_syn] non-fatal error occurred syscall accept(2), code:%d, link:%lld", errno, ncb_server->hld);
                *ctrlcode = errno;
                break;

        /* ERRORs: (in the any of the following cases, the listening service link will be automatic destroy)
            EBADFD      The sockfd is not an open file descriptor
            EFAULT      The addr argument is not in a writable part of the user address space
            EINVAL      Socket is not listening for connections, or addrlen is invalid (e.g., is negative), or invalid value in falgs
            ENOTSOCK    The file descriptor sockfd does not refer to a socket
            EOPNOTSUPP  The referenced socket is not of type SOCK_STREAM.
            EPROTO      Protocol error. */
            default:
                nis_call_ecr("[nshost.tcpio.__tcp_syn] fatal error occurred syscall accept(2), error:%d, link:%lld", errno, ncb_server->hld);
                *ctrlcode = -1;
                break;
        }
        return -1;
    }

    nis_call_ecr("[nshost.tcpio.__tcp_syn_try] accepted socket:%d", *clientfd);
    return 0;
}

static
int __tcp_syn_dpc(ncb_t *ncb_server, ncb_t *ncb)
{
    socklen_t addrlen;

    if (!ncb_server || !ncb) {
        return -EINVAL;
    }

    /* save local and remote address struct */
    addrlen = sizeof(struct sockaddr);
    getpeername(ncb->sockfd, (struct sockaddr *) &ncb->remot_addr, &addrlen); /* remote */
    getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen); /* local */

    /* set other options */
    tcp_update_opts(ncb);

    /* acquire save TCP Info and adjust linger in the accept phase.
        l_onoff on and l_linger not zero, these settings means:
        TCP drop any data cached in the kernel buffer of this socket file descriptor when close(2) called.
        post a TCP-RST to peer, do not use FIN-FINACK, using this flag to avoid TIME_WAIT stauts */
    ncb_set_linger(ncb, 1, 0);

    /* allocate memory for TCP normal package */
    if (NULL == (ncb->packet = (unsigned char *) malloc(TCP_BUFFER_SIZE))) {
        return -ENOMEM;
    }

    /* clear the protocol head */
    ncb->u.tcp.rx_parse_offset = 0;
    if (NULL == (ncb->u.tcp.rx_buffer = (unsigned char *) malloc(TCP_BUFFER_SIZE))) {
        return -ENOMEM;
    }

    /* specify data handler proc for client ncb object */
    posix__atomic_set(&ncb->ncb_read, &tcp_rx);
    posix__atomic_set(&ncb->ncb_write, &tcp_tx);

    /* copy the context from listen fd to accepted one in needed */
    if (ncb_server->attr & LINKATTR_TCP_UPDATE_ACCEPT_CONTEXT) {
        ncb->attr = ncb_server->attr;
        memcpy(&ncb->u.tcp.template, &ncb_server->u.tcp.template, sizeof(tst_t));
    }

    /* attach to epoll as early as it can to ensure the EPOLLRDHUP and EPOLLERR event not be lost,
        BUT do NOT allow the EPOLLIN event, because receive message should NOT early than accepted message */
    if (io_attach(ncb, 0) < 0) {
        return -1;
    }

    /* tell calling thread, link has been accepted.
        user can rewrite some context in callback even if LINKATTR_TCP_UPDATE_ACCEPT_CONTEXT is set */
    ncb_post_accepted(ncb_server, ncb->hld);

    /* allow the EPOLLIN event to visit this file-descriptor */
    io_modify(ncb, EPOLLIN);
    return 0;
}

static
int __tcp_syn(ncb_t *ncb_server)
{
    ncb_t *ncb;
    objhld_t hld;
    struct tcp_info ktcp;
    int retval;
    int clientfd;
    int ctrlcode;

    retval = 0;
    clientfd = -1;
    ctrlcode = -1;

    /* get the socket status of tcp_info to check the socket tcp statues,
        it must be listen states when accept syscall */
    if (tcp_save_info(ncb_server, &ktcp) >= 0) {
        if (ktcp.tcpi_state != TCP_LISTEN) {
            nis_call_ecr("[nshost.tcpio.__tcp_syn] state illegal,link:%lld, kernel states %s.", ncb_server->hld, tcp_state2name(ktcp.tcpi_state));
            return 0;
        }
    }

    /* try syscall connect(2) once, if accept socket fatal, the ncb object willbe destroy */
    if ( (retval = __tcp_syn_try(ncb_server, &clientfd, &ctrlcode)) >= 0) {
        hld = objallo(sizeof ( ncb_t), &ncb_allocator, &ncb_deconstruct, NULL, 0);
        if (hld < 0) {
            close(clientfd);
            return 0;
        }
        ncb = objrefr(hld);
        assert(ncb);
        ncb->sockfd = clientfd;
        ncb->hld = hld;

        ncb->protocol = IPPROTO_TCP;
        ncb->nis_callback = ncb_server->nis_callback;

        /* initial the client ncb object, link willbe destroy on fatal. */
        if (__tcp_syn_dpc(ncb_server, ncb) < 0) {
            objclos(hld);
        }
        objdefr(hld);
        return 0;
    }

    return ctrlcode;
}

int tcp_syn(ncb_t *ncb_server)
{
    int retval;

    do {
        retval = __tcp_syn(ncb_server);
    } while (0 == retval);
    return retval;
}

static
int __tcp_rx(ncb_t *ncb)
{
    int recvcb;
    int overplus;
    int offset;
    int cpcb;

    recvcb = recv(ncb->sockfd, ncb->u.tcp.rx_buffer, TCP_BUFFER_SIZE, 0);
    if (recvcb > 0) {
        cpcb = recvcb;
        overplus = recvcb;
        offset = 0;
        do {
            overplus = tcp_parse_pkt(ncb, ncb->u.tcp.rx_buffer + offset, cpcb);
            if (overplus < 0) {
                /* fatal to parse low level protocol,
                    close the object immediately */
                return -1;
            }
            offset += (cpcb - overplus);
            cpcb = overplus;
        } while (overplus > 0);
    }

    /* a stream socket peer has performed an orderly shutdown */
    if (0 == recvcb) {
        nis_call_ecr("[nshost.tcpio.__tcp_rx] fatal error occurred syscall recv(2), the return value equal to zero, link:%lld", ncb->hld );
        return -ECONNRESET;
    }

    /* ECONNRESET 104 Connection reset by peer */
    if (recvcb < 0) {

        /* A signal occurred before any data  was  transmitted, try again by next loop */
        if (errno == EINTR) {
            return 0;
        }

        /* no more data canbe read, waitting for next epoll edge trigger */
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
            return EAGAIN;
        }

        nis_call_ecr("[nshost.tcpio.__tcp_rx] fatal error occurred syscall recv(2), error:%d, link:%lld", errno, ncb->hld );
        return -1;
    }

    return 0;
}

int tcp_rx(ncb_t *ncb)
{
    int retval;

    /* read receive buffer until it's empty */
    while (0 == (retval = __tcp_rx(ncb)))
        ;
    return retval;
}

int tcp_txn(ncb_t *ncb, void *p)
{
    int wcb;
    struct tx_node *node;

    node = (struct tx_node *)p;

    while (node->offset < node->wcb) {
        wcb = send(ncb->sockfd, node->data + node->offset, node->wcb - node->offset, 0);

        /* fatal-error/connection-terminated  */
        if (0 == wcb) {
            nis_call_ecr("[nshost.tcpio.tcp_txn] fatal error occurred syscall send(2), the return value equal to zero, link:%lld", ncb->hld );
            return -1;
        }

        if (wcb < 0) {
            /* the write buffer is full, active EPOLLOUT and waitting for epoll event trigger
             * at this point, we need to deal with the queue header node and restore the unprocessed node back to the queue header.
             * the way 'oneshot' focus on the write operation completion point */
            if (EAGAIN == errno) {
                nis_call_ecr("[nshost.tcpio.tcp_txn] syscall send(2) would block cause by kernel memory overload,link:%lld", ncb->hld);
                return -EAGAIN;
            }

            /* A signal occurred before any data  was  transmitted
                continue and send again */
            if (EINTR == errno) {
                continue;
            }

            /* other error, these errors should cause link close */
            nis_call_ecr("[nshost.tcpio.tcp_txn] fatal error occurred syscall send(2), error:%d, link:%lld",errno, ncb->hld );
            return -1;
        }

        node->offset += wcb;
    }

    return node->wcb;
}

/* TCP sender proc */
int tcp_tx(ncb_t *ncb)
{
    struct tx_node *node;
    struct tcp_info ktcp;

    if (!ncb) {
        return -1;
    }

    /* get the socket status of tcp_info to check the socket tcp statues */
    if (tcp_save_info(ncb, &ktcp) >= 0) {
        if (ktcp.tcpi_state != TCP_ESTABLISHED) {
            nis_call_ecr("[nshost.tcpio.tcp_tx] state illegal,link:%lld, kernel states:%s.", ncb->hld, tcp_state2name(ktcp.tcpi_state));
            return -1;
        }
    }

    /* try to write front package into system kernel send-buffer */
    if (fifo_top(ncb, &node) >= 0) {
        return tcp_txn(ncb, node);
    }

    return 0;
}

#if 0
static int __tcp_poll_syn(int sockfd, int *err)
{
    struct pollfd pofd;
    socklen_t errlen;
    int error;

    pofd.fd = sockfd;
    pofd.events = POLLOUT;
    errlen = sizeof(error);

    if (!err) {
        return -EINVAL;
    }

    do {
        if (poll(&pofd, 1, -1) < 0) {
            break;
        }

        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &errlen) < 0) {
            break;
        }

        *err = error;
        return ((0 == error) ? (0) : (-1));

    } while (0);

    *err = errno;
    return -1;
}
#endif

static int __tcp_check_syn_result(int sockfd, int *err)
{
    socklen_t errlen;
    int error;

    error = 0;
    errlen = sizeof(error);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &errlen) < 0) {
        error = errno;
    }

    *err = error;
    return ((0 == error) ? (0) : (-1));
}

/*
 * tcp connect request asynchronous completed handler
 */
int tcp_tx_syn(ncb_t *ncb)
{
    int e;
    socklen_t addrlen;

    while (1) {
        if( 0 == __tcp_check_syn_result(ncb->sockfd, &e)) {
            tcp_update_opts(ncb);

            /* get peer address information */
            addrlen = sizeof (struct sockaddr);
            getpeername(ncb->sockfd, (struct sockaddr *) &ncb->remot_addr, &addrlen); /* remote address information */
            getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen); /* local address information */

            /* focus EPOLLIN only */
            if (io_modify(ncb, EPOLLIN) < 0) {
                objclos(ncb->hld);
                return -1;
            }

            /* follow tcp rx/tx event */
            posix__atomic_set(&ncb->ncb_read, &tcp_rx);
            posix__atomic_set(&ncb->ncb_write, &tcp_tx);

            nis_call_ecr("[nshost.tcp.tcp_tx_syn] link:%lld connection established.", ncb->hld);
            ncb_post_connected(ncb);
            return 0;
        }

        switch (e) {
            /* connection has been establish or already existed */
            case EISCONN:
            case EALREADY:
                return 0;

            /* other interrupted or full cached,try again
                Only a few linux version likely to happen. */
            case EINTR:
                break;

            case EAGAIN:
                return -EAGAIN;

            /* Connection refused
             * ulimit -n overflow(open file cout lg then 1024 in default) */
            case ECONNREFUSED:
            default:
                nis_call_ecr("[nshost.tcpio.tcp_tx_syn] fatal error occurred syscall poll(2), error:%d, link %lld.", e, ncb->hld);
                return -1;
        }
    }

    return 0;
}

/*
 * tcp connect request asynchronous error handler
 */
int tcp_rx_syn(ncb_t *ncb)
{
    int error;
    socklen_t errlen;
    int retval;

    if (!ncb) {
        return -1;
    }

    error = 0;
    errlen = sizeof(error);
    if (0 == (retval = getsockopt(ncb->sockfd, SOL_SOCKET, SO_ERROR, &error, &errlen))) {
        if (0 == error) {
            return 0;
        }
        nis_call_ecr("[nshost.tcpio.tcp_rx_syn] error by syscall getsockopt(2), error:%d,link:%lld", error, ncb->hld);
    } else {
        nis_call_ecr("[nshost.tcpio.tcp_rx_syn] fatal error occurred syscall getsockopt(2), error:%d,link:%lld", errno, ncb->hld);
    }

    return -1;
}
