#include "ncb.h"
#include "mxx.h"
#include "fifo.h"
#include "io.h"

#include <pthread.h>

static LIST_HEAD(nl_head);
static pthread_mutex_t nl_head_locker = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static int nl_count = 0;

static void ncb_post_preclose(const ncb_t *ncb);
static void ncb_post_closed(const ncb_t *ncb);

/* ncb uninit proc will dereference all ncb object and try to going to close phase.
 */
void ncb_uninit(int protocol)
{
    ncb_t *ncb;
    struct list_head *root, *cursor, *n;
    int i;
    int nl_count_proto;
    objhld_t *hlds;

    root = &nl_head;
    hlds = NULL;
    nl_count_proto = 0;

    /* duplicate all pending objects, and than try to close it */
    pthread_mutex_lock(&nl_head_locker);
    do {
        if (nl_count <= 0 ) {
            break;
        }

        hlds = (objhld_t *)malloc(nl_count * sizeof(objhld_t));
        if (!hlds) {
            break;
        }

        list_for_each_safe(cursor, n, root) {
            ncb = containing_record(cursor, ncb_t, nl_entry);
            if (ncb->protocol == protocol) {
                list_del(&ncb->nl_entry);
                INIT_LIST_HEAD(&ncb->nl_entry);
                hlds[nl_count_proto] = ncb->hld;
                nl_count_proto++;
            }
        }
    } while(0);
    pthread_mutex_unlock(&nl_head_locker);

    if (hlds && nl_count_proto > 0) {
        for (i = 0 ; i < nl_count_proto; i++) {
            nis_call_ecr("[nshost.ncb.ncb_uninit] link:%lld close by ncb uninit", ncb->hld);
            objclos(hlds[i]);
        }
        free(hlds);
    }
}

int ncb_allocator(void *udata, const void *ctx, int ctxcb)
{
    ncb_t *ncb;

    ncb = (ncb_t *)udata;
    assert(ncb);
    if (!ncb) {
        return -EINVAL;
    }

    memset(ncb, 0, sizeof (ncb_t));
    fifo_init(ncb);

    /* insert this ncb node into gloabl nl_head */
    pthread_mutex_lock(&nl_head_locker);
    list_add_tail(&ncb->nl_entry, &nl_head);
    nl_count++;
    pthread_mutex_unlock(&nl_head_locker);
    return 0;
}

void ncb_deconstruct(objhld_t ignore, void *p)
{
    ncb_t *ncb;

    ncb = (ncb_t *) p;
    assert(ncb);
    if (!ncb) {
        return;
    }

    /* post pre close event to calling thread, and than,
        Invalidate the user context pointer, trust calling has been already handled and free @ncb->context  */
    ncb_post_preclose(ncb);
    ncb->context = NULL;

    /* stop network service
     * cancel relation of epoll
     * close file descriptor */
    io_close(ncb);

    /* free packet cache */
    if (ncb->packet) {
        free(ncb->packet);
        ncb->packet = NULL;
    }

    if (ncb->u.tcp.rx_buffer && ncb->protocol == IPPROTO_TCP) {
        free(ncb->u.tcp.rx_buffer);
        ncb->u.tcp.rx_buffer = NULL;

        if (ncb_lb_marked(ncb)) {
            free(ncb->u.tcp.lbdata);
        }
        ncb->u.tcp.lbdata = NULL;
        ncb->u.tcp.lbsize = 0;
        ncb->u.tcp.lboffset = 0;
    }

    /* clear all packages pending in send queue */
    fifo_uninit(ncb);

    /* remove entry from global nl_head */
    pthread_mutex_lock(&nl_head_locker);
    list_del(&ncb->nl_entry);
    INIT_LIST_HEAD(&ncb->nl_entry);
    assert(nl_count > 0);
    if (nl_count > 0) {
        nl_count--;
    }
    pthread_mutex_unlock(&nl_head_locker);

    /* post close event to calling thread */
    ncb_post_closed(ncb);

    /* set callback function to ineffectiveness */
    ncb->nis_callback = NULL;

    nis_call_ecr("[nshost.ncb.ncb_deconstruct] link:%lld finalization released",ncb->hld);
}

int ncb_set_rcvtimeo(const ncb_t *ncb, const struct timeval *timeo)
{
    return (ncb && timeo) ?
            setsockopt(ncb->sockfd, SOL_SOCKET, SO_RCVTIMEO, (const void *)timeo, sizeof(struct timeval)) : -EINVAL;
}

int ncb_get_rcvtimeo(const ncb_t *ncb)
{
    socklen_t optlen;
    if (ncb) {
        optlen = sizeof(ncb->rcvtimeo);
        return getsockopt(ncb->sockfd, SOL_SOCKET, SO_RCVTIMEO, (void *__restrict)&ncb->rcvtimeo, &optlen);
    }
    return -EINVAL;
}

int ncb_set_sndtimeo(const ncb_t *ncb, const struct timeval *timeo)
{
    return (ncb && timeo) ?
            setsockopt(ncb->sockfd, SOL_SOCKET, SO_SNDTIMEO, (const void *)timeo, sizeof(struct timeval)) : -EINVAL;
}

int ncb_get_sndtimeo(const ncb_t *ncb)
{
    socklen_t optlen;

    if (ncb) {
        optlen = sizeof(ncb->sndtimeo);
        return getsockopt(ncb->sockfd, SOL_SOCKET, SO_SNDTIMEO, (void *__restrict)&ncb->sndtimeo, &optlen);
    }
    return -EINVAL;
}

int ncb_set_iptos(const ncb_t *ncb, int tos)
{
    unsigned char type_of_service = (unsigned char )tos;

    if (ncb && type_of_service) {
        return setsockopt(ncb->sockfd, SOL_IP, IP_TOS, (const void *)&type_of_service, sizeof(type_of_service));
    }
    return -EINVAL;
}

int ncb_get_iptos(const ncb_t *ncb)
{
    socklen_t optlen;
    if (ncb) {
        optlen = sizeof(ncb->iptos);
        return getsockopt(ncb->sockfd, SOL_IP, IP_TOS, (void *__restrict)&ncb->iptos, &optlen);
    }
    return -EINVAL;
}

int ncb_set_window_size(const ncb_t *ncb, int dir, int size)
{
    return (NULL != ncb) ?
            setsockopt(ncb->sockfd, SOL_SOCKET, dir, (const void *)&size, sizeof(size)) : -EINVAL;
}

int ncb_get_window_size(const ncb_t *ncb, int dir, int *size)
{
    socklen_t optlen;
    if (ncb && size) {
        optlen = sizeof(int);
        return getsockopt(ncb->sockfd, SOL_SOCKET, dir, (void *__restrict)size, &optlen);
    }

     return -EINVAL;
}

int ncb_set_linger(const ncb_t *ncb, int onoff, int lin)
{
    struct linger lgr;

    if (!ncb){
        return -EINVAL;
    }

    lgr.l_onoff = onoff;
    lgr.l_linger = lin;
    return setsockopt(ncb->sockfd, SOL_SOCKET, SO_LINGER, (char *) &lgr, sizeof ( struct linger));
}

int ncb_get_linger(const ncb_t *ncb, int *onoff, int *lin)
{
    struct linger lgr;
    socklen_t optlen;

    if (!ncb) {
        return -EINVAL;
    }

    optlen = sizeof (lgr);
    if (getsockopt(ncb->sockfd, SOL_SOCKET, SO_KEEPALIVE, (void *__restrict) & lgr, &optlen) < 0) {
        return -1;
    }

    if (onoff) {
        *onoff = lgr.l_onoff;
    }

    if (lin){
        *lin = lgr.l_linger;
    }

    return 0;
}

static void ncb_post_preclose(const ncb_t *ncb)
{
    nis_event_t c_event;
    tcp_data_t c_data;

    if (ncb) {
        if (ncb->nis_callback) {
            c_event.Ln.Tcp.Link = ncb->hld;
            c_event.Event = EVT_PRE_CLOSE;
            c_data.e.PreClose.Context = ncb->context;
            ncb->nis_callback(&c_event, &c_data);
        }
    }
}

static void ncb_post_closed(const ncb_t *ncb)
{
    nis_event_t c_event;

    if (ncb) {
        if (ncb->nis_callback) {
            c_event.Ln.Tcp.Link = ncb->hld;
            c_event.Event = EVT_CLOSED;
            ncb->nis_callback(&c_event, NULL);
        }
    }
}

void ncb_post_recvdata(const ncb_t *ncb,  int cb, const unsigned char *data)
{
    nis_event_t c_event;
    tcp_data_t c_data;

    if (ncb) {
        if (ncb->nis_callback) {
            c_event.Ln.Tcp.Link = (HTCPLINK) ncb->hld;
            c_event.Event = EVT_RECEIVEDATA;
            c_data.e.Packet.Size = cb;
            c_data.e.Packet.Data = data;
            ncb->nis_callback(&c_event, &c_data);
        }
    }
}

void ncb_post_pipedata(const ncb_t *ncb,  int cb, const unsigned char *data)
{
    nis_event_t c_event;
    tcp_data_t c_data;

    if (ncb) {
        if (ncb->nis_callback) {
            c_event.Ln.Tcp.Link = (HTCPLINK) ncb->hld;
            c_event.Event = EVT_PIPEDATA;
            c_data.e.Packet.Size = cb;
            c_data.e.Packet.Data = data;
            ncb->nis_callback(&c_event, &c_data);
        }
    }
}

void ncb_post_accepted(const ncb_t *ncb, HTCPLINK link)
{
    nis_event_t c_event;
    tcp_data_t c_data;

    if (ncb) {
        if (ncb->nis_callback) {
            c_event.Event = EVT_TCP_ACCEPTED;
            c_event.Ln.Tcp.Link = ncb->hld;
            c_data.e.Accept.AcceptLink = link;
            ncb->nis_callback(&c_event, &c_data);
        }
    }
}

void ncb_post_connected(const ncb_t *ncb)
{
    nis_event_t c_event;

    if (ncb) {
        if (ncb->nis_callback) {
            c_event.Event = EVT_TCP_CONNECTED;
            c_event.Ln.Tcp.Link = ncb->hld;
            ncb->nis_callback(&c_event, NULL);
        }
    }
}
