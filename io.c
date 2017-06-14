#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

#include <linux/unistd.h>
#include <linux/types.h>

#include "posix_types.h"
#include "posix_thread.h"
#include "posix_atomic.h"
#include "posix_wait.h"
#include "posix_ifos.h"

#include "io.h"
#include "ncb.h"
#include "object.h"
#include "worker.h"
#include "clist.h"

/* 1024 is just a hint for the kernel */
#define EPOLL_SIZE    (1024)

struct event_node {
    int evt_count_;
    struct epoll_event events_[EPOLL_SIZE];
    struct list_head link_;
};

static struct {
    int fd_;
    posix__boolean_t epoll_actived_;
    posix__pthread_t epoll_thread_; /* EPOLL 线程 */
    struct list_head event_head_; /* struct event_node 组成的事件延迟处理队列 */
    posix__pthread_mutex_t event_lock_;
    posix__boolean_t delay_actived_;
    posix__waitable_handle_t delay_waiter_; /* 唤醒延迟处理线程 */
    posix__pthread_t delay_thread_; /* 延迟处理线程 */
} epoll_object;

static int refcnt = 0;

#define USE_IO_DPC (0)

uint64_t otime = 0;

static void io_run(struct epoll_event *evts, int sigcnt){
    int i;
    objhld_t hld;
    ncb_t *ncb;
    
    for (i = 0; i < sigcnt; i++) {
        hld = evts[i].data.fd;
        if (hld < 0) {
            continue;
        }

        if (evts[i].events & EPOLLRDHUP) {
            post_task(hld, kTaskType_Destroy);
            continue;
        }

        /*
         * 触发条件:
         * 1. 有数据到达
         * 2. 有 syn 请求到达
         * 3. 读取缓冲区因任意可能转变为非空
         * TCP 读缓冲区  cat /proc/sys/net/ipv4/tcp_rmem 
         */
        if (evts[i].events & EPOLLIN) {
            post_task(hld, kTaskType_RxOrder);
        }

        /*
         * 注意事项:
         * 1. EPOLLOUT 一旦被关注， 则每个写入缓冲区不满 EPOLLIN 都会携带触发一次
         * 2. 平常无需关注 EPOLLOUT
         * 3. 一旦写入操作发生 EAGAIN, 则下一个写入操作能且只能由 EPOLLOUT 发起(关注状态切换)
         * TCP 写缓冲区 cat /proc/sys/net/ipv4/tcp_wmem 
         */
        if (evts[i].events & EPOLLOUT) {
            
            printf("EPOLLOUT  %llu\n", posix__clock_gettime());
            
            ncb = objrefr(hld);
            if (ncb) {
                /* EPOLLOUT 到达， 解除该对象的 IO 阻止
                 *  解除对象对 EPOLLOUT 的关注 */
                iordonly(ncb, hld);

                /* 投递写任务 */
                post_task(hld, kTaskType_TxOrder);

                objdefr(hld);
            }
        }
    }
}

static void *delay_proc(void *argv) {
    int sigcnt;
    struct epoll_event *evts;
    struct event_node *e_node;

    while (epoll_object.delay_actived_) {
        if (posix__waitfor_waitable_handle(&epoll_object.delay_waiter_, -1) < 0) {
            break;
        }

        while (1) {
            posix__pthread_mutex_lock(&epoll_object.event_lock_);
            if (NULL == (e_node = list_first_entry_or_null(&epoll_object.event_head_, struct event_node, link_))) {
                posix__pthread_mutex_unlock(&epoll_object.event_lock_);
                break;
            }
            list_del(&e_node->link_);
            posix__pthread_mutex_unlock(&epoll_object.event_lock_);

            sigcnt = e_node->evt_count_;
            evts = &e_node->events_[0];

            io_run(evts, sigcnt);
            free(e_node);
        }
    }
    
    return NULL;
}

static void *epoll_proc(void *argv) {
    struct epoll_event evts[EPOLL_SIZE];
    int sigcnt;
    int errcode;
    struct event_node *e_node;

    while (epoll_object.epoll_actived_) {
        printf("epoll wait: %llu\n", posix__clock_gettime());
#if USE_IO_DPC
        e_node = (struct event_node *) malloc(sizeof (struct event_node));
        assert(e_node);
         
        e_node->evt_count_ = epoll_wait(epoll_object.fd_, e_node->events_, EPOLL_SIZE, -1);
        if ( e_node->evt_count_ < 0) {
#else
        sigcnt = epoll_wait(epoll_object.fd_, evts, EPOLL_SIZE, -1);
        
        printf("epoll awaken: %llu\n", posix__clock_gettime());
        
        if (sigcnt < 0) {
#endif
            errcode = errno;

            /* EINTR表示被更高级的系统调用打断，包括一次recv无法完成的缓冲区接收 */
            if (EINTR == errcode || EAGAIN == errcode) {
                continue;
            }
            printf("[EPOLL] error on epoll_wait, errno=%d.\n", errcode);
            break;
        }
        
        printf("epoll working: %llu\n", posix__clock_gettime());
        
#if USE_IO_DPC
        posix__pthread_mutex_lock(&epoll_object.event_lock_);
        list_add_tail(&e_node->link_, &epoll_object.event_head_);
        posix__pthread_mutex_unlock(&epoll_object.event_lock_);
        posix__sig_waitable_handle(&epoll_object.delay_waiter_);
#else
        io_run(evts, sigcnt);
#endif
    }

    printf("[EPOLL] services trunk loop terminated.\n");
    return NULL;
}

int ioinit() {
    int retval;

    if (posix__atomic_inc(&refcnt) > 1) {
        return 0;
    }

    /* 对一个已经关闭的链接执行 write, 返回 EPIPE 的同时会 raise 一个SIGPIPE 信号，需要忽略处理 */
    signal(SIGPIPE, SIG_IGN);

    epoll_object.fd_ = epoll_create(EPOLL_SIZE);
    if (epoll_object.fd_ < 0) {
        printf("[EPOLL] failed to allocate file descriptor.\n");
        return -1;
    }

    do {
        retval = -1;

        INIT_LIST_HEAD(&epoll_object.event_head_);

        /* 创建 EPOLL 的 IO 线程 */
        epoll_object.epoll_actived_ = posix__true;
        if (posix__pthread_create(&epoll_object.epoll_thread_, &epoll_proc, NULL) < 0) {
            break;
        }

        /* 创建 EPOLL 的 BH 线程 */
        epoll_object.delay_actived_ = posix__true;
        if (posix__init_synchronous_waitable_handle(&epoll_object.delay_waiter_) < 0) {
            break;
        }
        if (posix__pthread_create(&epoll_object.epoll_thread_, &delay_proc, NULL) < 0) {
            break;
        }

        posix__pthread_mutex_init(&epoll_object.event_lock_);
        retval = 0;
    } while (0);

    if (retval < 0) {
        close(epoll_object.fd_);

        epoll_object.epoll_actived_ = posix__false;
        posix__pthread_join(&epoll_object.epoll_thread_, NULL);
        epoll_object.delay_actived_ = posix__false;
        posix__pthread_join(&epoll_object.delay_thread_, NULL);
        
        posix__uninit_waitable_handle(&epoll_object.delay_waiter_);
    }
    return retval;
}

void iouninit() {
    struct event_node *enode;

    if (0 == refcnt) {
        return;
    }

    /* 需要所有初始化调用者都认可反初始化， 反初始化才能得以执行 */
    if (posix__atomic_dec(&refcnt) > 0) {
        return;
    }

    /* EPOLL 描述符如果已经打开， 则关闭 */
    if (epoll_object.fd_ > 0) {
        close(epoll_object.fd_);
        epoll_object.fd_ = -1;
    }

    /* IO 对象被激活， 则结束 */
    if (epoll_object.epoll_actived_) {
        posix__atomic_xchange(&epoll_object.epoll_actived_, posix__false);
        posix__pthread_join(&epoll_object.epoll_thread_, NULL);
        return;
    }

    if (epoll_object.delay_actived_) {
        posix__atomic_xchange(&epoll_object.delay_actived_, posix__false);
        posix__sig_waitable_handle(&epoll_object.delay_waiter_);
        posix__pthread_join(&epoll_object.epoll_thread_, NULL);
        posix__uninit_waitable_handle(&epoll_object.delay_waiter_);
    }

    /* 清空所有的未处理 EPOLL 事件节点 */
    posix__pthread_mutex_lock(&epoll_object.event_lock_);
    while (NULL != (enode = list_first_entry_or_null(&epoll_object.event_head_, struct event_node, link_))) {
        list_del(&enode->link_);
        free(enode);
    }
    posix__pthread_mutex_unlock(&epoll_object.event_lock_);
    INIT_LIST_HEAD(&epoll_object.event_head_);
    posix__pthread_mutex_release(&epoll_object.event_lock_);
}

int ioatth(int fd, int hld) {
    struct epoll_event e_evt;

    if (-1 == epoll_object.fd_) {
        return -1;
    }

    e_evt.data.fd = hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP | EPOLLIN); /* | EPOLLOUT);*/
    return epoll_ctl(epoll_object.fd_, EPOLL_CTL_ADD, fd, &e_evt);
}

int iordonly(void *ncbptr, int hld){
    ncb_t *ncb;
    struct epoll_event e_evt;

    ncb = (ncb_t *)ncbptr;
    
    if (!ncb || (-1 == epoll_object.fd_)){
        return -EINVAL;
    }
    
    ncb_cancel_wblock(ncb);

    e_evt.data.fd = hld;
    e_evt.events = EPOLLET | EPOLLRDHUP | EPOLLIN;
    return epoll_ctl(epoll_object.fd_, EPOLL_CTL_MOD, ncb->sockfd, &e_evt);
}

int iordwr(void *ncbptr, int hld){
    struct epoll_event e_evt;
    ncb_t *ncb;

    ncb = (ncb_t *)ncbptr;
    
    if (!ncb || (-1 == epoll_object.fd_)) {
        return -1;
    }
    
    ncb_mark_wblocked(ncb);

    e_evt.data.fd = hld;
    e_evt.events = EPOLLET | EPOLLRDHUP | EPOLLIN | EPOLLOUT;
    return epoll_ctl(epoll_object.fd_, EPOLL_CTL_MOD, ncb->sockfd, &e_evt);
}

int iodeth(int fd) {
    struct epoll_event evt;
    return epoll_ctl(epoll_object.fd_, EPOLL_CTL_DEL, fd, &evt);
}

int setasio(int fd) {
    int opt;

    if (fd < 0) {
        return -1;
    }

    opt = fcntl(fd, F_GETFL);
    if (opt < 0) {
        printf("[EPOLL] failed get file status flag,errno=%d.\n ", errno);
        return -1;
    }

    if (fcntl(fd, F_SETFL, opt | O_NONBLOCK) < 0) {
        printf("[EPOLL] failed set file status flag with non_block,errno=%d.\n", errno);
        return -1;
    }
    return 0;
}

int setsyio(int fd){
    int opt;

    if (fd < 0) {
        return -1;
    }

    opt = fcntl(fd, F_GETFL);
    if (opt < 0) {
        printf("[EPOLL] failed get file status flag,errno=%d.\n ", errno);
        return -1;
    }

    opt &= ~O_NONBLOCK;
    if (fcntl(fd, F_SETFL, opt) < 0) {
        printf("[EPOLL] failed set file status flag with syio,errno=%d.\n", errno);
        return -1;
    }
    return 0;
}