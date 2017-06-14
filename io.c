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
    posix__pthread_t epoll_thread_; /* EPOLL �߳� */
    struct list_head event_head_; /* struct event_node ��ɵ��¼��ӳٴ������ */
    posix__pthread_mutex_t event_lock_;
    posix__boolean_t delay_actived_;
    posix__waitable_handle_t delay_waiter_; /* �����ӳٴ����߳� */
    posix__pthread_t delay_thread_; /* �ӳٴ����߳� */
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
         * ��������:
         * 1. �����ݵ���
         * 2. �� syn ���󵽴�
         * 3. ��ȡ���������������ת��Ϊ�ǿ�
         * TCP ��������  cat /proc/sys/net/ipv4/tcp_rmem 
         */
        if (evts[i].events & EPOLLIN) {
            post_task(hld, kTaskType_RxOrder);
        }

        /*
         * ע������:
         * 1. EPOLLOUT һ������ע�� ��ÿ��д�뻺�������� EPOLLIN ����Я������һ��
         * 2. ƽ�������ע EPOLLOUT
         * 3. һ��д��������� EAGAIN, ����һ��д���������ֻ���� EPOLLOUT ����(��ע״̬�л�)
         * TCP д������ cat /proc/sys/net/ipv4/tcp_wmem 
         */
        if (evts[i].events & EPOLLOUT) {
            
            printf("EPOLLOUT  %llu\n", posix__clock_gettime());
            
            ncb = objrefr(hld);
            if (ncb) {
                /* EPOLLOUT ��� ����ö���� IO ��ֹ
                 *  �������� EPOLLOUT �Ĺ�ע */
                iordonly(ncb, hld);

                /* Ͷ��д���� */
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

            /* EINTR��ʾ�����߼���ϵͳ���ô�ϣ�����һ��recv�޷���ɵĻ��������� */
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

    /* ��һ���Ѿ��رյ�����ִ�� write, ���� EPIPE ��ͬʱ�� raise һ��SIGPIPE �źţ���Ҫ���Դ��� */
    signal(SIGPIPE, SIG_IGN);

    epoll_object.fd_ = epoll_create(EPOLL_SIZE);
    if (epoll_object.fd_ < 0) {
        printf("[EPOLL] failed to allocate file descriptor.\n");
        return -1;
    }

    do {
        retval = -1;

        INIT_LIST_HEAD(&epoll_object.event_head_);

        /* ���� EPOLL �� IO �߳� */
        epoll_object.epoll_actived_ = posix__true;
        if (posix__pthread_create(&epoll_object.epoll_thread_, &epoll_proc, NULL) < 0) {
            break;
        }

        /* ���� EPOLL �� BH �߳� */
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

    /* ��Ҫ���г�ʼ�������߶��Ͽɷ���ʼ���� ����ʼ�����ܵ���ִ�� */
    if (posix__atomic_dec(&refcnt) > 0) {
        return;
    }

    /* EPOLL ����������Ѿ��򿪣� ��ر� */
    if (epoll_object.fd_ > 0) {
        close(epoll_object.fd_);
        epoll_object.fd_ = -1;
    }

    /* IO ���󱻼�� ����� */
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

    /* ������е�δ���� EPOLL �¼��ڵ� */
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