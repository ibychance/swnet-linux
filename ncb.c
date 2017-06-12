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
        
        /* 直接收包的队列初始化 */
        INIT_LIST_HEAD(&ncb->userio_list_);
        posix__pthread_mutex_init(&ncb->userio_lock_);

        fque_init(&ncb->packet_fifo_);

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

    if (ncb->fd_ > 0) {
        shutdown(ncb->fd_, 2);
        close(ncb->fd_);
        ncb->fd_ = -1;
    }

    if (ncb->packet_ && ncb->packet_size_ > 0) {
        free(ncb->packet_);
        ncb->packet_ = NULL;
    }

    if (ncb->recv_buffer_) {
        free(ncb->recv_buffer_);
        ncb->recv_buffer_ = NULL;
    }

    /*释放用户上下文内存*/
    if (ncb->user_context_ && ncb->user_context_size_ > 0) {
        free(ncb->user_context_);
        ncb->user_context_ = NULL;
        ncb->user_context_size_ = 0;
    }

    if (ncb->user_callback_ && ncb->hld_ >= 0) {
        c_event.Ln.Tcp.Link = ncb->hld_;
        c_event.Event = EVT_CLOSED;
        c_data.e.LinkOption.OptionLink = ncb->hld_;
        ncb->user_callback_(&c_event, &c_data);
    }

    /*清空所有仍在缓冲区的未发送包, 这里没有线程安全问题 */
    fque_uninit(&ncb->packet_fifo_);
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
    if (ncb->user_callback_) {
        ncb->user_callback_(&c_event, &c_data);
    }
}

void ncb_post_user_task(ncb_t *ncb, struct user_event_node_t *event){
    posix__pthread_mutex_lock(&ncb->userio_lock_);
    list_add_tail(&event->link, &ncb->userio_list_);

    /* 为了保证到达包的线性原则，每个ncb必须保证只有一个任务处于解析状态 */
    if (!ncb->if_userio_running_) {
        ncb->if_userio_running_ = posix__true;
        post_task(ncb->hld_, kTaskType_User);
    }
    posix__pthread_mutex_unlock(&ncb->userio_lock_);
}