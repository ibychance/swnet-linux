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
        
        posix__pthread_mutex_init(&ncb->rx_prot_lock);
        ncb->rx_order_count = 0;
        ncb->rx_running = posix__false;
        
        fque_init(&ncb->tx_fifo);

        /* IO 阻止标志， 初始化为 "非阻止" */
        ncb->write_io_blocked_ = posix__false;
        return 0;
    }

    return -EINVAL;
}

void ncb_uninit(int ignore, void *p) {
    nis_event_t c_event;
    tcp_data_t c_data;
    ncb_t *ncb;

    if (!p) {
        return;
    }

    ncb = (ncb_t *) p;

    if (ncb->sockfd > 0) {
        shutdown(ncb->sockfd, 2);
        close(ncb->sockfd);
        ncb->sockfd = -1;
    }

    if (ncb->packet) {
        free(ncb->packet);
        ncb->packet = NULL;
    }

    if (ncb->rx_buffer) {
        free(ncb->rx_buffer);
        ncb->rx_buffer = NULL;
    }

    /*释放用户上下文内存*/
    if (ncb->context && ncb->context_size > 0) {
        free(ncb->context);
        ncb->context = NULL;
        ncb->context_size = 0;
    }

    if (ncb->nis_callback && ncb->hld_ >= 0) {
        c_event.Ln.Tcp.Link = ncb->hld_;
        c_event.Event = EVT_CLOSED;
        c_data.e.LinkOption.OptionLink = ncb->hld_;
        ncb->nis_callback(&c_event, &c_data);
    }

    /*清空所有仍在缓冲区的未发送包, 这里没有线程安全问题 */
    fque_uninit(&ncb->tx_fifo);
    
    posix__pthread_mutex_uninit(&ncb->rx_prot_lock);
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

    c_event.Ln.Udp.Link = ncb->hld_;
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