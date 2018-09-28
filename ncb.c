#include "ncb.h"
#include "nis.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "posix_thread.h"
#include "posix_string.h"

int ncb_init(ncb_t *ncb) {

    if (ncb) {
        memset(ncb, 0, sizeof (ncb_t));
        
        fque_init(&ncb->tx_fifo);

        /* flag of write IO block, initialize status is not blocking */
        ncb->write_io_blocked = posix__false;
        return 0;
    }

    return RE_ERROR(EINVAL);
}

void ncb_uninit(objhld_t ignore, void *p) {
    ncb_t *ncb;

    if (!p) {
        return;
    }

    ncb = (ncb_t *) p;

    /* post pre close event to calling thread */
    if (ncb->hld >= 0) {
        ncb_post_preclose(ncb);
    }
    
    /* stop network service
     * cancel relation of epoll 
     * close file descriptor */
    ioclose(ncb);
    
    /* free packet cache */
    if (ncb->packet) {
        free(ncb->packet);
        ncb->packet = NULL;
    }
    if (ncb->rx_buffer) {
        free(ncb->rx_buffer);
        ncb->rx_buffer = NULL;
    }

    /* free context of user data */
    if (ncb->context && ncb->context_size > 0) {
        free(ncb->context);
        ncb->context = NULL;
        ncb->context_size = 0;
    }

    /* clear all packages pending in send queue */
    fque_uninit(&ncb->tx_fifo);
    
    /* post close event to calling thread */
    if (ncb->hld >= 0) {
        ncb_post_close(ncb);
    }

    /* set callback function to ineffectiveness */
    ncb->nis_callback = NULL;
}

void ncb_report_debug_information(ncb_t *ncb, const char *fmt, ...) {
    udp_data_t c_data;
    nis_event_t c_event;
    char logstr[128];
    va_list ap;
    int retval;

    if (!ncb || !fmt) {
        return;
    }

    c_event.Ln.Udp.Link = ncb->hld;
    c_event.Event = EVT_DEBUG_LOG;

    va_start(ap, fmt);
    retval = posix__vsprintf(logstr, cchof(logstr), fmt, ap);
    va_end(ap);

    if (retval <= 0) {
        return;
    }
    logstr[retval] = 0;

    c_data.e.DebugLog.logstr = &logstr[0];
    if (ncb->nis_callback) {
        ncb->nis_callback(&c_event, &c_data);
    }
}

int ncb_set_rcvtimeo(ncb_t *ncb, struct timeval *timeo){
    if (ncb && timeo > 0){
        return setsockopt(ncb->sockfd, SOL_SOCKET, SO_RCVTIMEO, (const void *)timeo, sizeof(struct timeval));
    }
    return RE_ERROR(EINVAL);
}

int ncb_get_rcvtimeo(ncb_t *ncb){
    if (ncb){
         socklen_t optlen =sizeof(ncb->rcvtimeo);
        return getsockopt(ncb->sockfd, SOL_SOCKET, SO_RCVTIMEO, (void *__restrict)&ncb->rcvtimeo, &optlen);
    }
    return RE_ERROR(EINVAL);
}

int ncb_set_sndtimeo(ncb_t *ncb, struct timeval *timeo){
    if (ncb && timeo > 0){
        return setsockopt(ncb->sockfd, SOL_SOCKET, SO_SNDTIMEO, (const void *)timeo, sizeof(struct timeval));
    }
    return RE_ERROR(EINVAL);
}

int ncb_get_sndtimeo(ncb_t *ncb){
    if (ncb){
        socklen_t optlen =sizeof(ncb->sndtimeo);
        return getsockopt(ncb->sockfd, SOL_SOCKET, SO_SNDTIMEO, (void *__restrict)&ncb->sndtimeo, &optlen);
    }
    return RE_ERROR(EINVAL);
}

int ncb_set_iptos(ncb_t *ncb, int tos){
    unsigned char type_of_service = (unsigned char )tos;
    if (ncb && type_of_service){
        return setsockopt(ncb->sockfd, SOL_IP, IP_TOS, (const void *)&type_of_service, sizeof(type_of_service));
    }
    return RE_ERROR(EINVAL);
}

int ncb_get_iptos(ncb_t *ncb){
    if (ncb){
        socklen_t optlen =sizeof(ncb->iptos);
        return getsockopt(ncb->sockfd, SOL_IP, IP_TOS, (void *__restrict)&ncb->iptos, &optlen);
    }
    return RE_ERROR(EINVAL);
}

int ncb_set_window_size(ncb_t *ncb, int dir, int size){
    if (ncb){
        return setsockopt(ncb->sockfd, SOL_SOCKET, dir, (const void *)&size, sizeof(size));
    }
    
     return RE_ERROR(EINVAL);
}

int ncb_get_window_size(ncb_t *ncb, int dir, int *size){
    if (ncb && size){
        socklen_t optlen = sizeof(int);
        if (getsockopt(ncb->sockfd, SOL_SOCKET, dir, (void *__restrict)size, &optlen) < 0){
            return -1;
        }
    }
    
     return RE_ERROR(EINVAL);
}

int ncb_set_linger(ncb_t *ncb, int onoff, int lin){
    struct linger lgr;
    
    if (!ncb){
        return RE_ERROR(EINVAL);
    }
    
    lgr.l_onoff = onoff;
    lgr.l_linger = lin;
    return setsockopt(ncb->sockfd, SOL_SOCKET, SO_LINGER, (char *) &lgr, sizeof ( struct linger));
}

int ncb_get_linger(ncb_t *ncb, int *onoff, int *lin) {
    struct linger lgr;
    socklen_t optlen = sizeof (lgr);

    if (!ncb) {
        return RE_ERROR(EINVAL);
    }

    if (getsockopt(ncb->sockfd, SOL_SOCKET, SO_KEEPALIVE, (void *__restrict) & lgr, &optlen) < 0) {
        return -1;
    }

    if (onoff){
        *onoff = lgr.l_onoff;
    }
    
    if (lin){
        *lin = lgr.l_linger;
    }
    
    return 0;
}

void ncb_post_preclose(ncb_t *ncb) {
    nis_event_t c_event;
    tcp_data_t c_data;

    if (ncb) {
        if (ncb->nis_callback) {
            c_event.Ln.Tcp.Link = ncb->hld;
            c_event.Event = EVT_PRE_CLOSE;
            c_data.e.LinkOption.OptionLink = ncb->hld;
            ncb->nis_callback(&c_event, &c_data);
        }
    }
}

void ncb_post_close(ncb_t *ncb) {
    nis_event_t c_event;
    tcp_data_t c_data;

    if (ncb) {
        if (ncb->nis_callback) {
            c_event.Ln.Tcp.Link = ncb->hld;
            c_event.Event = EVT_CLOSED;
            c_data.e.LinkOption.OptionLink = ncb->hld;
            ncb->nis_callback(&c_event, &c_data);
        }
    }
}

void ncb_post_recvdata(ncb_t *ncb,  int cb, const char *data) {
    nis_event_t c_event;
    tcp_data_t c_data;

    if (ncb) {
        if (ncb->nis_callback) {
            c_event.Ln.Tcp.Link = (HTCPLINK) ncb->hld;
            c_event.Event = EVT_RECEIVEDATA;
            c_data.e.Packet.Size = cb;
            c_data.e.Packet.Data = (const char *) ((char *) data);
            ncb->nis_callback(&c_event, &c_data);
        }
    }
}

void ncb_post_accepted(ncb_t *ncb, HTCPLINK link) {
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

void ncb_post_senddata(ncb_t *ncb,  int cb, const char *data) {
    nis_event_t c_event;
    tcp_data_t c_data;

    if (ncb) {
        if (ncb->nis_callback) {
            c_event.Ln.Tcp.Link = (HTCPLINK) ncb->hld;
            c_event.Event = EVT_SENDDATA;
            c_data.e.Packet.Size = cb;
            c_data.e.Packet.Data = data;
            ncb->nis_callback(&c_event, &c_data);
        }
    }
}

void ncb_post_connected(ncb_t *ncb) {
    nis_event_t c_event;
    tcp_data_t c_data;

    if (ncb) {
        if (ncb->nis_callback) {
            c_event.Event = EVT_TCP_CONNECTED;
            c_event.Ln.Tcp.Link = ncb->hld;
            c_data.e.LinkOption.OptionLink = ncb->hld;
            ncb->nis_callback(&c_event, &c_data);
        }
    } 
}
