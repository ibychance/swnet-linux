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

struct epoll_object_t{
    int epfd;
    posix__boolean_t actived;
    posix__pthread_t thread; /* EPOLL �߳� */
} ;

static struct epoll_object_t epoll_object;
static int refcnt = 0;

static void io_run(struct epoll_event *evts, int sigcnt){
    int i;
    objhld_t hld;
    
    for (i = 0; i < sigcnt; i++) {
        hld = evts[i].data.fd;
        if (hld < 0) {
            continue;
        }

        if (evts[i].events & EPOLLRDHUP) {
            post_read_task(hld, kTaskType_Destroy);
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
            //printf("[%u] io EPOLLIN %llu\n", posix__gettid(), posix__clock_gettime());
            post_read_task(hld, kTaskType_RxOrder);
        }

        /*
         * ETģʽ�£�EPOLLOUT���������У�
            1.��������-->������������
            2.ͬʱ����EPOLLOUT��EPOLLIN�¼� ʱ������IN �¼�����������˳��һ��OUT�¼���
            3.һ���ͻ���connect������accept�ɹ���ᴥ��һ��OUT�¼���

         * ע������:
         * 1. (EPOLLIN | EPOLLOUT) һ������ע�� ��ÿ��д�뻺�������� EPOLLIN ����Я������һ��, ������ܣ� �Ҳ����ײ��� oneshot
         * 2. ƽ�������ע EPOLLOUT
         * 3. һ��д��������� EAGAIN, ����һ��д���������ֻ���� EPOLLOUT ����(��ע״̬�л�)
         * TCP д������ cat /proc/sys/net/ipv4/tcp_wmem 
         */
        if (evts[i].events & EPOLLOUT) {
             //printf("[%u] io EPOLLOUT %llu\n", posix__gettid(), posix__clock_gettime());
            post_write_task(hld, kTaskType_TxOrder);
        }
    }
}

static void *epoll_proc(void *argv) {
    struct epoll_event evts[EPOLL_SIZE];
    int sigcnt;
    int errcode;

    while (epoll_object.actived) {
        sigcnt = epoll_wait(epoll_object.epfd, evts, EPOLL_SIZE, -1);
        if (sigcnt < 0) {
            errcode = errno;

            /* EINTR��ʾ�����߼���ϵͳ���ô�ϣ�����һ��recv�޷���ɵĻ��������� */
            if (EINTR == errcode || EAGAIN == errcode) {
                continue;
            }
            printf("[EPOLL] error on epoll_wait, errno=%d.\n", errcode);
            break;
        }
        io_run(evts, sigcnt);
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

    epoll_object.epfd = epoll_create(EPOLL_SIZE);
    if (epoll_object.epfd < 0) {
        printf("[EPOLL] failed to allocate file descriptor.\n");
        return -1;
    }

    do {
        retval = -1;
        
        /* ���� EPOLL �� IO �߳� */
        epoll_object.actived = posix__true;
        if (posix__pthread_create(&epoll_object.thread, &epoll_proc, NULL) < 0) {
            break;
        }
        
        retval = 0;
    } while (0);

    if (retval < 0) {
        close(epoll_object.epfd);
        epoll_object.actived = posix__false;
        posix__pthread_join(&epoll_object.thread, NULL);
    }
    return retval;
}

void iouninit() {
    
    if (0 == refcnt) {
        return;
    }

    /* ��Ҫ���г�ʼ�������߶��Ͽɷ���ʼ���� ����ʼ�����ܵ���ִ�� */
    if (posix__atomic_dec(&refcnt) > 0) {
        return;
    }

    /* EPOLL ����������Ѿ��򿪣� ��ر� */
    if (epoll_object.epfd > 0) {
        close(epoll_object.epfd);
        epoll_object.epfd = -1;
    }

    /* IO ���󱻼�� ����� */
    if (epoll_object.actived) {
        posix__atomic_xchange(&epoll_object.actived, posix__false);
        posix__pthread_join(&epoll_object.thread, NULL);
        return;
    }
}

int ioatth(void *ncbptr, enum io_poll_mask_t mask) {
    struct epoll_event e_evt;
    ncb_t *ncb;

    if (-1 == epoll_object.epfd) {
        return -1;
    }
    
    ncb = (ncb_t *)ncbptr;

    e_evt.data.fd = ncb->hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP); 
    if (mask & kPollMask_Oneshot) {
        e_evt.events |= EPOLLONESHOT;
    }
    if (mask & kPollMask_Read) {
        e_evt.events |= EPOLLIN;
    }
    if (mask & kPollMask_Write){
        e_evt.events |= EPOLLOUT;
    }
    return epoll_ctl(epoll_object.epfd, EPOLL_CTL_ADD, ncb->sockfd, &e_evt);
}

int iomod(void *ncbptr, enum io_poll_mask_t mask ) {
    struct epoll_event e_evt;
    ncb_t *ncb;

    if (-1 == epoll_object.epfd) {
        return -1;
    }
    
    ncb = (ncb_t *)ncbptr;

    e_evt.data.fd = ncb->hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP | EPOLLIN); 
    if (mask & kPollMask_Oneshot) {
        e_evt.events |= EPOLLONESHOT;
    }
    if (mask & kPollMask_Read) {
        e_evt.events |= EPOLLIN;
    }
    if (mask & kPollMask_Write){
        e_evt.events |= EPOLLOUT;
    }
    return epoll_ctl(epoll_object.epfd, EPOLL_CTL_MOD, ncb->sockfd, &e_evt);
}

int iodeth(int fd) {
    struct epoll_event evt;
    return epoll_ctl(epoll_object.epfd, EPOLL_CTL_DEL, fd, &evt);
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