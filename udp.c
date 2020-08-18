#include "udp.h"

#include "mxx.h"
#include "fifo.h"
#include "io.h"
#include "wpool.h"
#include "pipe.h"

#include "posix_atomic.h"

static
int __udprefr( objhld_t hld, ncb_t **ncb )
{
    if ( hld < 0 || !ncb) {
        return -EINVAL;
    }

    *ncb = objrefr( hld );
    if ( NULL != (*ncb) ) {
        if ( (*ncb)->protocol == IPPROTO_UDP ) {
            return 0;
        }

        objdefr( hld );
        *ncb = NULL;
        return -EPROTOTYPE;
    }

    return -ENOENT;
}

static int __udp_update_opts(ncb_t *ncb)
{
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

int udp_init()
{
	int retval;

	retval = io_init(IPPROTO_UDP);
	if (0 != retval) {
		return retval;
	}

    retval = wp_init(IPPROTO_UDP);
    if (retval < 0) {
        io_uninit(IPPROTO_UDP);
    }

    return retval;
}

void udp_uninit()
{
    ncb_uninit(IPPROTO_UDP);
    io_uninit(IPPROTO_UDP);
    wp_uninit(IPPROTO_UDP);
}

HUDPLINK udp_create(udp_io_callback_t callback, const char* ipstr, uint16_t port, int flag)
{
    int fd;
    struct sockaddr_in addrlocal;
    int retval;
    int optval;
    objhld_t hld;
    socklen_t addrlen;
    ncb_t *ncb;

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        nis_call_ecr("[nshost.udp.create] fatal error occurred syscall socket(2), error:%d", errno);
        return -1;
    }

    optval = 1;
    retval = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof ( optval));

    addrlocal.sin_addr.s_addr = ipstr ? inet_addr(ipstr) : INADDR_ANY;
    addrlocal.sin_family = AF_INET;
    addrlocal.sin_port = htons(port);
    retval = bind(fd, (struct sockaddr *) &addrlocal, sizeof ( struct sockaddr));
    if (retval < 0) {
        nis_call_ecr("[nshost.udp.create] fatal error occurred syscall bind(2),local endpoint %s:%u, error:%d,", (ipstr ? ipstr : "0.0.0.0"), port, errno);
        close(fd);
        return -1;
    }

    hld = objallo(sizeof ( ncb_t), &ncb_allocator, &ncb_deconstruct, NULL, 0);
    if (hld < 0) {
        nis_call_ecr("[nshost.udp.create] insufficient resource for allocate inner object");
        close(fd);
        return -1;
    }
    ncb = (ncb_t *) objrefr(hld);
    assert(ncb);

    do {
        /* copy initialize parameters */
        ncb->nis_callback = callback;
        ncb->sockfd = fd;
        ncb->hld = hld;
        ncb->protocol = IPPROTO_UDP;

        /* setsockopt */
        if (__udp_update_opts(ncb) < 0) {
            break;
        }

        /* allocate buffer for normal packet */
        if (NULL == (ncb->packet = (unsigned char *) malloc(MAX_UDP_UNIT))) {
            retval = -ENOMEM;
            break;
        }

        /* extension of broadcast/multicast */
        if (flag & UDP_FLAG_BROADCAST) {
            if (udp_set_boardcast(ncb, 1) < 0) {
                break;
            }
            ncb->attr |= UDP_FLAG_BROADCAST;
        } else {
            if (flag & UDP_FLAG_MULTICAST) {
                ncb->attr |= UDP_FLAG_MULTICAST;
            }
        }

        /* get local address info */
        addrlen = sizeof(ncb->local_addr);
        getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen);

        /* set data handler function pointer for Rx/Tx */
        posix__atomic_set(&ncb->ncb_read, &udp_rx);
        posix__atomic_set(&ncb->ncb_write, &udp_tx);

        /* attach to epoll */
        retval = io_attach(ncb, EPOLLIN);
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

void udp_destroy(HUDPLINK link)
{
    ncb_t *ncb;

    /* it should be the last reference operation of this object no matter how many ref-count now. */
    ncb = objreff(link);
    if (ncb) {
        nis_call_ecr("[nshost.udp.destroy] link:%lld order to destroy", ncb->hld);
        io_close(ncb);
        objdefr(link);
    }
}

int udp_awaken(HUDPLINK link, const void *pipedata, int cb)
{
    int retval;
    ncb_t *ncb;

    if (link < 0) {
        return -EINVAL;
    }

    retval = __udprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    retval = pipe_write_message(ncb, pipedata, cb);

    objdefr(link);
    return retval;
}

int udp_write(HUDPLINK link, const void *origin, int cb, const char* ipstr, uint16_t port, const nis_serializer_t serializer)
{
    int retval;
    ncb_t *ncb;
    unsigned char *buffer;
    struct tx_node *node;

    if ( !ipstr || (0 == port) || (cb <= 0) || (link < 0) || (cb > MAX_UDP_UNIT) || !origin) {
        return -EINVAL;
    }

    retval = -1;
    buffer = NULL;
    node = NULL;

    retval = __udprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    do {
        retval = -1;

        if (NULL == (buffer = (unsigned char *) malloc(cb))) {
            retval = -ENOMEM;
            break;
        }

        /* serialize data into packet or direct use data pointer by @origin */
        if (serializer) {
            if ((*serializer)(buffer, origin, cb) < 0 ) {
                break;
            }
        } else {
            memcpy(buffer, origin, cb);
        }

        if (NULL == (node = (struct tx_node *) malloc(sizeof (struct tx_node)))) {
            retval = -ENOMEM;
            break;
        }
        memset(node, 0, sizeof(struct tx_node));
        node->data = buffer;
        node->wcb = cb;
        node->offset = 0;
        node->udp_target.sin_family = AF_INET;
        node->udp_target.sin_addr.s_addr = inet_addr(ipstr);
        node->udp_target.sin_port = htons(port);

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

int udp_getaddr(HUDPLINK link, uint32_t *ipv4, uint16_t *port)
{
    ncb_t *ncb;
    int retval;

    retval = __udprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    if (ipv4) {
        *ipv4 = htonl(ncb->local_addr.sin_addr.s_addr);
    }
    if (port) {
        *port = htons(ncb->local_addr.sin_port);
    }

    objdefr(link);
    return retval;
}

int udp_setopt(HUDPLINK link, int level, int opt, const char *val, int len)
{
    ncb_t *ncb;
    int retval;

    retval = __udprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    retval = setsockopt(ncb->sockfd, level, opt, val, len);

    objdefr(link);
    return retval;
}

int udp_getopt(HUDPLINK link, int level, int opt, char *val, int *len)
{
    ncb_t *ncb;
    int retval;

    retval = __udprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    retval = getsockopt(ncb->sockfd, level, opt, val, (socklen_t *)len);

    objdefr(link);
    return retval;
}

int udp_set_boardcast(ncb_t *ncb, int enable)
{
    return ncb ? setsockopt(ncb->sockfd, SOL_SOCKET, SO_BROADCAST, (const void *) &enable, sizeof (enable)) : -EINVAL;
}

int udp_get_boardcast(ncb_t *ncb, int *enabled)
{
    socklen_t optlen;

    if (ncb && enabled) {
        optlen = sizeof (int);
        return getsockopt(ncb->sockfd, SOL_SOCKET, SO_BROADCAST, (void * __restrict)enabled, &optlen);
    }
    return -EINVAL;
}

/*
* The destination address of multicast message uses Class D IP address. Class D address cannot appear in the source IP address field of IP message.
* In the process of unicast data transmission, a data packet transmission path is routed from the source address to the destination address,
* which is transmitted in the IP network using the "hop-by-hop" principle.
*
* However, in the IP multicast ring, the destination address of the packet is not one, but a group, forming a group address.
* All information receivers join a group, and once they join, the data flowing to the group address begins to be transmitted to the receivers immediately,
* and all members of the group can receive the data packet.
*
* The members of multicast group are dynamic, and the host can join and leave the multicast group at any time.
*
* All hosts receiving multicast packets with the same IP multicast address constitute a host group, also known as multicast group.
* The membership of a multicast group changes at any time. A host can join or leave the multiple group at any time.
* The number and location of the members of the multicast group are unrestricted. A host can also belong to several multiple groups.
*
* In addition, hosts that do not belong to a multicast group can also send data packets to the multicast group.
*
* multicast addressing:
* Multicast groups can be permanent or temporary. In multicast group addresses, some of them are officially assigned, which is called permanent multicast group.
*
* Permanent multicast group keeps its IP address unchanged, and its membership can change.
* The number of members in a permanent multicast group can be arbitrary or even zero.
* IP multicast addresses that are not reserved for permanent multicast groups can be used by temporary multicast groups.
*       224.0.0.0-224.0.0.255 is the reserved multicast address (permanent group address). The address 224.0.0.0 is reserved without allocation.
*                                Other addresses are used by routing protocols.
*       224.0.1.0-224.0.1.255 is a public multicast address that can be used on the Internet.
*       224.0.2.0-238.255.255.255 is user-available multicast address (temporary group address), which is valid throughout the network.
*       239.0.0.0-239.255.255.255 is a locally managed multicast address, which is valid only within a specific local range.
*
* Multicast is a one-to-many transmission mode, in which there is a concept of multicast group.
* The sender sends data to a group. The router in the network automatically sends data to all terminals listening to the group through the underlying IGMP protocol.
*
* As for broadcasting, there are some similarities with multicast.
* The difference is that the router sends a packet to every terminal in the subnet, whether or not these terminals are willing to receive the packet.
* UDP broadcasting can only be effective in the intranet (the same network segment), while multicast can better achieve cross-network segment mass data.
*
* UDP multicast is a connectionless and datagram connection mode, so it is unreliable.
* That is to say, whether the data can reach the receiving end and the order of data arrival are not guaranteed.
* But because UDP does not guarantee the reliability of data, all data transmission efficiency is very fast.
*/
int udp_joingrp(HUDPLINK link, const char *ipstr, uint16_t port)
{
    ncb_t *ncb;
    int retval;

    if (link < 0 || !ipstr || 0 == port) {
        return -EINVAL;
    }

    retval = __udprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    do {
        retval = -1;

        if (!(ncb->attr & UDP_FLAG_MULTICAST)) {
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
            if (NULL == (ncb->u.udp.mreq = (struct ip_mreq *)malloc(sizeof(struct ip_mreq)))) {
                break;
            }
        }
        ncb->u.udp.mreq->imr_multiaddr.s_addr = inet_addr(ipstr);
        ncb->u.udp.mreq->imr_interface.s_addr = ncb->local_addr.sin_addr.s_addr;
        retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const void *)ncb->u.udp.mreq, sizeof(struct ip_mreq));
        if (retval < 0){
            break;
        }

    } while (0);

    objdefr(link);
    return retval;
}

int udp_dropgrp(HUDPLINK link)
{
    ncb_t *ncb;
    int retval;

    retval = __udprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    do{
        retval = -1;

        if (!(ncb->attr & UDP_FLAG_MULTICAST) || !ncb->u.udp.mreq) {
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

    objdefr(link);
    return retval;
}

int udp_setattr_r(ncb_t *ncb, int attr)
{
    __sync_lock_test_and_set(&ncb->attr, attr);
    if (ncb->attr & LINKATTR_UDP_BAORDCAST) {
        return udp_set_boardcast(ncb, 1);
    } else {
        return udp_set_boardcast(ncb, 0);
    }
}

int udp_getattr_r(ncb_t *ncb, int *attr)
{
    return __sync_lock_test_and_set(attr, ncb->attr);
}
