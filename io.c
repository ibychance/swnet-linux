#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/signal.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

#include <linux/unistd.h>
#include <linux/types.h>

#include "compiler.h"
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

struct epoll_object {
    int epfd;
    posix__boolean_t actived;
    posix__pthread_t thread; /* EPOLL 线程 */
    int load; /* 此线程的当前负载压力情况 */
} ;

struct epoll_object_manager {
    struct epoll_object *epos;
    int divisions;		/* 分时多路复用中的链路个数 */
    posix__pthread_mutex_t lock_selection; /* 锁住最大/最小负载筛选及其下标更替 */ 
};

static struct epoll_object_manager epmgr;
static int refcnt = 0;

static void io_run(struct epoll_event *evts, int sigcnt){
    int i;
    ncb_t *ncb;
    objhld_t hld;
    
    for (i = 0; i < sigcnt; i++) {
        hld = evts[i].data.fd;
        ncb = (ncb_t *)objrefr(hld);
        if (!ncb) {
            continue;
        }

        /*
         * 断开事件
         */
        if (evts[i].events & EPOLLRDHUP) {
            objclos(ncb->hld);
        }

        /*
         * 触发条件:
         * 1. 有数据到达
         * 2. 有 syn 请求到达
         * 3. 读取缓冲区因任意可能转变为非空
         * 		TCP 读缓冲区  cat /proc/sys/net/ipv4/tcp_rmem 
         * 
         * 备注:
         * 1.9系列版本的最大特色就是epoll线程直接处理描述符的数据
         * 2.描述符从epoll唤醒到上层分析协议总共经历以下步骤:
         *      2.1 响应epoll事件，从内核缓冲区read直到EAGAIN
         *      2.2 应用层解包
         *      2.3 等待上层处理完成
         *   这其中，2.1和2.2， 无论是否转换线程，需要的计算时间都是相同，8系列的处理方式画蛇添足
         *   转换线程尽管可以表面上促进epoll获得下一个描述符事件，但收到事件后的处理消耗CPU时间并不会减少
         *   唯一需要关注的上层应用在 on_recvdata 中的操作方案， 如果上层采用耗时操作， 则会阻塞当前线程上所有描述符的继续收包
         * 3.考虑到多数的快速响应案例，即便上层可能发生阻塞操作，也应该由上层投递线程池，而不是由下层管理线程池
         */
        if (evts[i].events & EPOLLIN) {
            if (ncb->ncb_read) {
                if (ncb->ncb_read(ncb) < 0) {
                    objclos(ncb->hld);
                }
            }else{
                ncb_report_debug_information(ncb, "nullptr read address for EPOLLIN");
            }
        }

        /*
         * ET模式下，EPOLLOUT触发条件有：
         *   1.缓冲区满-->缓冲区非满；
		 *		TCP 写缓冲区 cat /proc/sys/net/ipv4/tcp_wmem 
         *   2.同时监听EPOLLOUT和EPOLLIN事件 时，当有IN 事件发生，都会顺带一个OUT事件；
         *   3.一个客户端connect过来，accept成功后会触发一次OUT事件。
		 *
         * 注意事项:
         * 1. (EPOLLIN | EPOLLOUT) 一旦被关注， 则每个写入缓冲区不满 EPOLLIN 都会携带触发一次, 损耗性能， 且不容易操作 oneshot
         * 2. 平常无需关注 EPOLLOUT
         * 3. 一旦写入操作发生 EAGAIN, 则下一个写入操作能且只能由 EPOLLOUT 发起(关注状态切换)
         * 
         * 
         * 已验证：当发生发送EAGAIN后再关注EPOLLOUT，可以在缓冲区成功空出后得到通知
         */
        if (evts[i].events & EPOLLOUT) {
            post_write_task(ncb->hld, kTaskType_TxOrder);
        }
        
        objdefr(hld);
    }
}

static void *epoll_proc(void *argv) {
    struct epoll_event evts[EPOLL_SIZE];
    int sigcnt;
    int errcode;
    struct epoll_object *epo;

    epo = (struct epoll_object *)argv;
    while (epo->actived) {
        sigcnt = epoll_wait(epo->epfd, evts, EPOLL_SIZE, -1);
        if (sigcnt < 0) {
            errcode = errno;

            /* EINTR表示被更高级的系统调用打断，包括一次recv无法完成的缓冲区接收 */
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
    int i;

    if (posix__atomic_inc(&refcnt) > 1) {
        return 0;
    }

    /* 对一个已经关闭的链接执行 write, 返回 EPIPE 的同时会 raise 一个SIGPIPE 信号，需要忽略处理 */
    signal(SIGPIPE, SIG_IGN);
    
    epmgr.divisions = get_nprocs() * 2;
    if ( NULL == (epmgr.epos = (struct epoll_object *)malloc(sizeof(struct epoll_object) * epmgr.divisions))) {
        posix__atomic_dec(&refcnt);
        return -1;
    }
    posix__pthread_mutex_init(&epmgr.lock_selection);
    
    for (i = 0; i < epmgr.divisions; i++) {
        epmgr.epos[i].load = 0;
        epmgr.epos[i].epfd = epoll_create(EPOLL_SIZE);
        if (epmgr.epos[i].epfd < 0) {
            printf("[EPOLL] failed to allocate file descriptor.\n");
            epmgr.epos[i].actived = posix__false;
            continue;
        }
        
        /* active 字段既作为运行有效性的判断符，也作为运行的控制符号 */
        epmgr.epos[i].actived = posix__true;
        if (posix__pthread_create(&epmgr.epos[i].thread, &epoll_proc, &epmgr.epos[i]) < 0) {
            epmgr.epos[i].actived = posix__false;
            close(epmgr.epos[i].epfd);
            epmgr.epos[i].epfd = -1;
        }
    }

    return 0;
}

void iouninit() {
    int i;
    
    if (0 == refcnt) {
        return;
    }

    /* 需要所有初始化调用者都认可反初始化， 反初始化才能得以执行 */
    if (posix__atomic_dec(&refcnt) > 0) {
        return;
    }
    
    if (!epmgr.epos) {
        return;
    }
    
    for (i = 0; i < epmgr.divisions; i++){
        if (epmgr.epos[i].epfd > 0){
            close(epmgr.epos[i].epfd);
            epmgr.epos[i].epfd = -1;
        }
        
        if (epmgr.epos[i].actived){
           posix__atomic_xchange(&epmgr.epos[i].actived, posix__false);
           posix__pthread_join(&epmgr.epos[i].thread, NULL);
        }
    }
    
    free(epmgr.epos);
    epmgr.epos = NULL;
    
    posix__pthread_mutex_uninit(&epmgr.lock_selection);
}

int ioatth(void *ncbptr, int mask) {
    struct epoll_event e_evt;
    ncb_t *ncb;
    
    ncb = (ncb_t *)ncbptr;
    if (!ncb) {
        return -EINVAL;
    }
    
    e_evt.data.fd = ncb->hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP); 
	e_evt.events |= mask;
	
	ncb->epfd = epmgr.epos[ncb->hld % epmgr.divisions].epfd;
    if ( epoll_ctl(ncb->epfd, EPOLL_CTL_ADD, ncb->sockfd, &e_evt) < 0 &&
			errno != EEXIST ) {
				ncb->epfd = -1;
				return -1;
	}
	return 0;
}

int iomod(void *ncbptr, int mask ) {
    struct epoll_event e_evt;
    ncb_t *ncb;
    
    ncb = (ncb_t *)ncbptr;
    if (!ncb) {
        return -EINVAL;
    }

    e_evt.data.fd = ncb->hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP ); 
	e_evt.events |= mask;
	
    return epoll_ctl(ncb->epfd, EPOLL_CTL_MOD, ncb->sockfd, &e_evt);
}

void iodeth(void *ncbptr) {
    struct epoll_event evt;
    ncb_t *ncb;
    
    ncb = (ncb_t *)ncbptr;
    if (ncb) {
        epoll_ctl(ncb->epfd, EPOLL_CTL_DEL, ncb->sockfd, &evt);
    }
}

void ioclose(void *ncbptr) {
    ncb_t *ncb = (ncb_t *)ncbptr;
    if (!ncb){
        return;
    }
    
    if (ncb->sockfd > 0){
         if (ncb->epfd> 0){
            iodeth(ncb);
            ncb->epfd = -1;
        }
         
        shutdown(ncb->sockfd, 2);
        close(ncb->sockfd);
        ncb->sockfd = -1;
    }
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