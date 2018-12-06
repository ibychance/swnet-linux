#include <stdio.h>

#include "tcp.h"
#include "mxx.h"
#include "posix_ifos.h"

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
            nis_call_ecr("nshost.tcpio.__tcp_syn:state illegal,link:%d, kernel states %s.",
                ncb_server->hld, TCP_KERNEL_STATE_NAME[ktcp.tcpi_state]);
            return 0;
        }
    }

    addrlen = sizeof ( addr_income);
    fd_client = accept(ncb_server->sockfd, (struct sockaddr *) &addr_income, &addrlen);
    errcode = errno;
    if (fd_client < 0) {

        /* syscall again if system interrupted */
        if (errcode == EINTR) {
            return 0;
        }

        /* no more data canbe read, waitting for next epoll edge trigger */
        if ((errcode == EAGAIN) || (errcode == EWOULDBLOCK)) {
            return EAGAIN;
        }

        /* the connection has been terminated before accept syscall in kernel.
            do NOT close the service link by this connection error. but nothing canbe continue for it */
        if (ECONNABORTED == errcode) {
            return 0;
        }

        /* for example: 
            EMFILE(24)  means there are too many file-descriptor opened/check user's own open file limit by ulimit -n(1024 by default)
            EBADFD(77)  the server sock fd has been closed before accept syscall but after epoll notify
            */
        nis_call_ecr("nshost.tcpio.__tcp_syn:accept syscall fatal with err:%d, link:%d", errcode, ncb_server->hld);
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

        /* tell calling thread, link has been accepted*/
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

        nis_call_ecr("nshost.tcpio.__tcp_rx: link %lld syscall recvfrom error, code:%d", ncb->hld, errcode);
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

static
int __tcp_tx(ncb_t *ncb, struct tx_node *node) {
    int wcb;
    int errcode;

    while (node->offset < node->wcb) {
        wcb = send(ncb->sockfd, node->data + node->offset, node->wcb - node->offset, 0);

        /* fatal-error/connection-terminated  */
        if (0 == wcb) {
            nis_call_ecr("nshost.tcpio.__tcp_tx: link %d zero bytes return by syscall send", ncb->hld);
            return -1;
        }

        if (wcb < 0) {
            errcode = errno;

            /* the write buffer is full, active EPOLLOUT and waitting for epoll event trigger
             * at this point, we need to deal with the queue header node and restore the unprocessed node back to the queue header.
             * the way 'oneshot' focus on the write operation completion point */
            if (EAGAIN == errcode) {
                return EAGAIN;
            }

            /* A signal occurred before any data  was  transmitted
                continue and send again */
            if (EINTR == errcode) {
                continue;
            }

            /* other error, these errors should cause link close */
            return RE_ERROR(errcode);
        }

        node->offset += wcb;
    }

    return 0;
}

/* TCP sender proc */
int tcp_tx(ncb_t *ncb) {
    struct tx_node *node;
    int retval;
    struct tcp_info ktcp;

    if (!ncb) {
        return -1;
    }

    /* get the socket status of tcp_info to check the socket tcp statues */
    if (tcp_save_info(ncb, &ktcp) >= 0) {
        if (ktcp.tcpi_state != TCP_ESTABLISHED) {
            nis_call_ecr("nshost.tcpio.tx:state illegal,link:%d, kernel states %s.", ncb->hld, TCP_KERNEL_STATE_NAME[ktcp.tcpi_state]);
            return -1;
        }
    }

    /* try to write all package into system kernel send-buffer */
    while (NULL != (node = fque_get(&ncb->tx_fifo))) {
        retval = __tcp_tx(ncb, node);
        if (retval < 0) {
            return retval;
        } else {
            if (EAGAIN == retval) {
                fque_revert(&ncb->tx_fifo, node);
                return EAGAIN;
            }
        }
        fque_free_node(node);
    }

    return 0;
}

static int __tcp_check_connection_bypoll(int sockfd, int *err) {
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
    int retval;
    int e;
    socklen_t addrlen;

    while (1) {
        retval = __tcp_check_connection_bypoll(ncb->sockfd, &e);
        if (retval >= 0) {
            /* set other options */
            tcp_update_opts(ncb);

            addrlen = sizeof (struct sockaddr);
            getpeername(ncb->sockfd, (struct sockaddr *) &ncb->remot_addr, &addrlen); /* remote address information */
            getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen); /* local address information */

            /* follow tcp rx/tx event */
            ncb->ncb_read = &tcp_rx;
            ncb->ncb_write = &tcp_tx;
            retval = iomod(ncb, EPOLLIN);
            if (retval < 0) {
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
             * Only a few linux version likely to happen. */
            case EINTR:
            case EAGAIN:
                nis_call_ecr("nshost.tcpio.syn:tcp syn retry.e=%d.", e);
                break;

            /* Connection refused
             * ulimit -n overflow(open file cout lg then 1024 in default) */
            case ECONNREFUSED:
            default:
                nis_call_ecr("nshost.tcpio.syn: fatal syscall, e=%d.", e);
                return -1;
        }
    }
    
    return -1;
}
