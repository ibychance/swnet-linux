#include "tcp.h"

#include <poll.h>

#include "mxx.h"
#include "fifo.h"
#include "io.h"

static
int __tcp_syn(ncb_t *ncb_server) {
    int fd_client;
    struct sockaddr_in addr_income;
    socklen_t addrlen;
    ncb_t *ncb_client;
    objhld_t hld_client;
    int errcode;
    struct tcp_info ktcp;

    /* get the socket status of tcp_info to check the socket tcp statues,
        it must be listen states when accept syscall */
    if (tcp_save_info(ncb_server, &ktcp) >= 0) {
        if (ktcp.tcpi_state != TCP_LISTEN) {
            nis_call_ecr("nshost.tcpio.__tcp_syn:state illegal,link:%lld, kernel states %s.",
                ncb_server->hld, TCP_KERNEL_STATE_NAME[ktcp.tcpi_state]);
            return 0;
        }
    }

    addrlen = sizeof ( addr_income);
    fd_client = accept(ncb_server->sockfd, (struct sockaddr *) &addr_income, &addrlen);
    errcode = errno;
    if (fd_client < 0) {

        /* The system call was interrupted by a signal that was caught before a valid connection arrived, or
            this connection has been aborted.
            in these case , this round of operation ignore, try next round accept notified by epoll */
        if ((errcode == EINTR) || (ECONNABORTED == errcode) ){
            return 0;
        }

        /* no more data canbe read, waitting for next epoll edge trigger */
        if ((errcode == EAGAIN) || (errcode == EWOULDBLOCK)) {
            return EAGAIN;
        }

        /* The per-process/system-wide limit on the number of open file descriptors has been reached, or
            Not enough free memory, or
            Firewall rules forbid connection.
            in these cases, this round of operation can fail, but the service link must be retain */
        if ((ENFILE == errcode) || (ENOBUFS == errcode) || (ENOMEM == errcode) || (EPERM == errcode)) {
            nis_call_ecr("nshost.tcpio.__tcp_syn:accept syscall throw warning code:%u, link:%lld", errcode, ncb_server->hld);
            return errcode;
        }

        /* ERRORs: (in the any of the following cases, the listening service link will be automatic destroy)
            EBADFD      The sockfd is not an open file descriptor
            EFAULT      The addr argument is not in a writable part of the user address space
            EINVAL      Socket is not listening for connections, or addrlen is invalid (e.g., is negative), or invalid value in falgs
            ENOTSOCK    The file descriptor sockfd does not refer to a socket
            EOPNOTSUPP  The referenced socket is not of type SOCK_STREAM.
            EPROTO      Protocol error. */
        nis_call_ecr("nshost.tcpio.__tcp_syn:accept syscall throw fatal error:%u, link:%lld", errcode, ncb_server->hld);
        return -1;
    }

    /* allocate ncb object for client */
    hld_client = objallo(sizeof ( ncb_t), NULL, &ncb_uninit, NULL, 0);
    if (hld_client < 0) {
        close(fd_client);
        return 0;
    }
    ncb_client = objrefr(hld_client);

    do {
        ncb_init(ncb_client);
        ncb_client->hld = hld_client;
        ncb_client->sockfd = fd_client;
        ncb_client->proto_type = kProtocolType_TCP;
        ncb_client->nis_callback = ncb_server->nis_callback;

        /* save local and remote address struct */
        getpeername(fd_client, (struct sockaddr *) &ncb_client->remot_addr, &addrlen); /* remote */
        getsockname(fd_client, (struct sockaddr *) &ncb_client->local_addr, &addrlen); /* local */

        /*all file descriptor must kept asynchronous with ET mode*/
        if (setasio(ncb_client->sockfd) < 0) {
            break;
        }

        /* set other options */
        tcp_update_opts(ncb_client);
        
        /* acquire save TCP Info and adjust linger in the accept phase. 
            l_onoff on and l_linger not zero, these settings means:
            TCP drop any data cached in the kernel buffer of this socket file descriptor when close(2) called.
            post a TCP-RST to peer, do not use FIN-FINACK, using this flag to avoid TIME_WAIT stauts */
        ncb_set_linger(ncb_client, 1, 0);

        /* allocate memory for TCP normal package */
        ncb_client->packet = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb_client->packet) {
            break;
        }

        /* clear the protocol head */
        ncb_client->u.tcp.rx_parse_offset = 0;
        ncb_client->u.tcp.rx_buffer = (char *) malloc(TCP_BUFFER_SIZE);
        if (!ncb_client->u.tcp.rx_buffer) {
            break;
        }

        /* specify data handler proc for client ncb object */
        ncb_client->ncb_read = &tcp_rx;
        ncb_client->ncb_write = &tcp_tx;

        /* copy the context from listen fd to accepted one in needed */
        if (ncb_server->u.tcp.attr & LINKATTR_TCP_UPDATE_ACCEPT_CONTEXT) {
            ncb_client->u.tcp.attr = ncb_server->u.tcp.attr;
            memcpy(&ncb_client->u.tcp.template, &ncb_server->u.tcp.template, sizeof(tst_t));
        }

        /* tell calling thread, link has been accepted. 
            user can rewrite some context in callback even if LINKATTR_TCP_UPDATE_ACCEPT_CONTEXT is set */
        ncb_post_accepted(ncb_server, hld_client);
        
        if (ioatth(ncb_client, EPOLLIN) < 0) {
            break;
        }

        objdefr(hld_client);
        return 0;
    } while (0);

    objdefr(hld_client);
    objclos(hld_client);
    return 0;
}

int tcp_syn(ncb_t *ncb_server) {
    int retval;

    do {
        retval = __tcp_syn(ncb_server);
    } while (0 == retval);
    return retval;
}

static
int __tcp_rx(ncb_t *ncb) {
    int recvcb;
    int overplus;
    int offset;
    int cpcb;
    int errcode;

    recvcb = recv(ncb->sockfd, ncb->u.tcp.rx_buffer, TCP_BUFFER_SIZE, 0);
    errcode = errno;
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
        nis_call_ecr("nshost.tcpio.__tcp_rx: link %lld zero bytes return by syscall recv", ncb->hld);
        return -1;
    }

    /* ECONNRESET 104 Connection reset by peer */
    if (recvcb < 0) {

        /* A signal occurred before any data  was  transmitted, try again by next loop */
        if (errcode == EINTR) {
            return 0;
        }

        /* no more data canbe read, waitting for next epoll edge trigger */
        if ((errcode == EAGAIN) || (errcode == EWOULDBLOCK)) {
            return EAGAIN;
        }

        nis_call_ecr("nshost.tcpio.__tcp_rx: link %lld syscall recv error, code:%d", ncb->hld, errcode);
        return -1;
    }
    return 0;
}

int tcp_rx(ncb_t *ncb) {
    int retval;

    /* read receive buffer until it's empty */
    do {
        retval = __tcp_rx(ncb);
    } while (0 == retval);
    return retval;
}

int tcp_txn(ncb_t *ncb, void *p) {
    int wcb;
    int errcode;
    struct tx_node *node;

    node = (struct tx_node *)p;

    while (node->offset < node->wcb) {
        wcb = send(ncb->sockfd, node->data + node->offset, node->wcb - node->offset, 0);

        /* fatal-error/connection-terminated  */
        if (0 == wcb) {
            nis_call_ecr("nshost.tcpio.tcp_txn: link %lld zero bytes return by syscall send", ncb->hld);
            return -1;
        }

        if (wcb < 0) {
            errcode = errno;

            /* the write buffer is full, active EPOLLOUT and waitting for epoll event trigger
             * at this point, we need to deal with the queue header node and restore the unprocessed node back to the queue header.
             * the way 'oneshot' focus on the write operation completion point */
            if (EAGAIN == errcode) {
                return -EAGAIN;
            }

            /* A signal occurred before any data  was  transmitted
                continue and send again */
            if (EINTR == errcode) {
                continue;
            }

            /* other error, these errors should cause link close */
            nis_call_ecr("nshost.tcpio.tcp_txn: link %lld error %d on syscall send",ncb->hld, errcode);
            return -1;
        }

        node->offset += wcb;
    }

    return node->wcb;
}

/* TCP sender proc */
int tcp_tx(ncb_t *ncb) {
    struct tx_node *node;
    struct tcp_info ktcp;

    if (!ncb) {
        return -1;
    }

    /* get the socket status of tcp_info to check the socket tcp statues */
    if (tcp_save_info(ncb, &ktcp) >= 0) {
        if (ktcp.tcpi_state != TCP_ESTABLISHED) {
            nis_call_ecr("nshost.tcpio.tcp_tx:state illegal,link:%lld, kernel states %s.", ncb->hld, TCP_KERNEL_STATE_NAME[ktcp.tcpi_state]);
            return -1;
        }
    }

    /* try to write front package into system kernel send-buffer */
    if (fifo_top(ncb, &node) >= 0) {
        return tcp_txn(ncb, node);
    }

    return 0;
}

static int __tcp_poll_syn(int sockfd, int *err) {
    struct pollfd pofd;
    socklen_t errlen;
    int error;

    pofd.fd = sockfd;
    pofd.events = POLLOUT;
    errlen = sizeof(error);

    if (!err) {
        return RE_ERROR(EINVAL);
    }

    do {
        if (poll(&pofd, 1, -1) < 0) {
            break;
        }

        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &errlen) < 0) {
            break;
        }

        *err = error;

        /* success only when SOL_ERROR return 0 */
        return ((0 == error) ? (0) : (-1));

    } while (0);

    *err = errno;
    return -1;
}

/*
 * tcp connect request asynchronous completed handler
 */
int tcp_tx_syn(ncb_t *ncb) {
    int e;
    socklen_t addrlen;

    while (1) {
        if( 0 == __tcp_poll_syn(ncb->sockfd, &e)) {
            /* set other options */
            tcp_update_opts(ncb);

            addrlen = sizeof (struct sockaddr);
            getpeername(ncb->sockfd, (struct sockaddr *) &ncb->remot_addr, &addrlen); /* remote address information */
            getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen); /* local address information */

            /* follow tcp rx/tx event */
            ncb->ncb_read = &tcp_rx;
            ncb->ncb_write = &tcp_tx;
            ncb->ncb_error = NULL;
            if (iomod(ncb, EPOLLIN) < 0) {
                objclos(ncb->hld);
                return -1;
            }

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
                nis_call_ecr("nshost.tcpio.tcp_tx_syn:link %lld connect request EAGAIN", ncb->hld);
                return -EAGAIN;

            /* Connection refused
             * ulimit -n overflow(open file cout lg then 1024 in default) */
            case ECONNREFUSED:
            default:
                nis_call_ecr("nshost.tcpio.tcp_tx_syn: fatal syscall, error:%d.", e);
                return -1;
        }
    }
    
    return 0;
}

/*
 * tcp connect request asynchronous error handler
 */
int tcp_rx_syn(ncb_t *ncb) {
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
        nis_call_ecr("nshost.tcpio.tcp_rx_syn:link %lld error %d", ncb->hld, error);
    } else {
        nis_call_ecr("nshost.tcpio.tcp_rx_syn:link %lld getsockopt error %d", ncb->hld, retval);
    }
    
    return -1;
}