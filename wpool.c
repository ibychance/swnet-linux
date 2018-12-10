#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/sysinfo.h>

#include "compiler.h"

#include "object.h"
#include "clist.h"
#include "ncb.h"
#include "wpool.h"
#include "tcp.h"
#include "mxx.h"

#include "posix_thread.h"
#include "posix_wait.h"
#include "posix_atomic.h"
#include "posix_ifos.h"

/*
 * 写入(发送)操作，从接口处无条件压入队列的弊端
 * 1. 如果多线程调用 tcp_write, 在应用层不控制顺序的前提下， nshost无法保障先后顺序按照调用线程的先后来执行
 * 2. 每个请求都入队且要锁，增加了线程切换开销， 尤其在发送缓冲区充裕的情况下， 完全没有必要
 * 
 * 在接口层进行 if_blocked 判断的策略:
 * 1, tcp_write 将直接调用 send, udp_write 将直接调用 sendto, 都直接使用调用线程的线程空间
 * 2, 如果系统调用发生 EAGAIN, 则置位该ncb的标记， 同时将未完成的发送数据缓冲区入队
 *     2.1 发生 EAGAIN 后， 在 blocked 标记清除前，tcp_write/udp_write 行为变更为直接入队
 *     2.2 EPOLL_OUT 事件响应后， 从缓冲队列先进先出取出待发送数据包，进行send/sendto 尝试， 直到全部缓冲包发送完毕或者再次 EAGAIN
 *     2.3 如果全部缓冲包发送完毕， 则重置 blocked 标记， 允许调用线程直接 send/sendto
 * 
 * 需要进行的测试项:
 * 1. 发生EAGAIN 后再关注 EPOLL_OUT 事件， 是否能确保事件被抓获
 */

struct wthread {
    posix__pthread_t thread;
    posix__pthread_mutex_t mutex;
    posix__waitable_handle_t signal;
    struct list_head tasks; /* struct task_node::link */
    int task_list_size;
};

struct task_node {
    objhld_t hld;
    enum task_type type;
    struct wthread *thread;
    struct list_head link;
};

struct wpool_manager {
    struct wthread *write_threads;
    int wthread_count;
    int stop;
};
static struct wpool_manager __wpool;

static void __add_task(struct task_node *task) {
    struct wthread *thread;
    if (task) {
        thread = task->thread;
        INIT_LIST_HEAD(&task->link);
        posix__pthread_mutex_lock(&thread->mutex);
        list_add_tail(&task->link, &thread->tasks);
        ++thread->task_list_size;
        posix__pthread_mutex_unlock(&thread->mutex);
    }
}

static struct task_node *__get_task(struct wthread *thread){
    struct task_node *task;
    
    posix__pthread_mutex_lock(&thread->mutex);
    if (NULL != (task = list_first_entry_or_null(&thread->tasks, struct task_node, link))) {
         --thread->task_list_size;
        list_del(&task->link);
        INIT_LIST_HEAD(&task->link);
    }
    posix__pthread_mutex_unlock(&thread->mutex);
    
    return task;
}

static int __run_task(struct task_node *task) {
    objhld_t hld;
    int retval;
    ncb_t *ncb;
    struct task_node *next;

    assert(NULL != task);
    
    hld = task->hld;
    
    ncb = objrefr(hld);
    if (!ncb) {
        return -1;
    }
    
    /* when the status is io blocked, Not responding to any task which type is kTaskType_TxTest */
    if (ncb_if_wblocked(ncb)) {
        if (task->type == kTaskType_TxTest) {
            objdefr(hld);
            return EAGAIN;
        }
    }
    
    /*
     * There are only two scenarios when the code goes here            
     * 1. No IO isolation occurred            
     * 2. IO isolation occurred, but kTaskType_TxOrder from EPOLLOUT was received and could respond normally. 
     */
    assert (ncb->ncb_write);
    if (!ncb->ncb_write) {
        return -1;
    }
    
    retval = ncb->ncb_write(ncb);

    /* fatal error cause by syscall, close this link */
    if(retval < 0){
        nis_call_ecr("nshost.wpool.task:write fr:%d, link %lld will be close", retval, ncb->hld);
        objclos(ncb->hld);
    }
    
    /* the queue of pending data node is empty, not any send operations are need.
        here can be consumed the task where allocated by kTaskType_TxOrder sucessful completed */
    else if (0 == retval) {
        ;
    }
    
    /* 
     * if EAGAIN happened, bacause the write operation object always takes place in the same thread context, there is no thread security problem. 
     * set write IO blocked. this ncb object willbe switch to focus on EPOLLOUT | EPOLLIN
     * for the data which has been reverted, Write tasks will be obtained through event triggering of EPOLLOUT
     */
    else if (EAGAIN == retval ) {
        if (!ncb_if_wblocked(ncb)) {
            ncb_mark_wblocked(ncb);
            iomod(ncb, EPOLLIN | EPOLLOUT);
            nis_call_ecr("nshost.wpool.task:link %lld mark to write blocked.", ncb->hld);
        }
    }
    
    /* @retval bytes have been written to kernel */
    else {
        if (task->type == kTaskType_TxOrder) {

            /* the IO-isolation is cancelled and EPOLL is switched to focus on only read/EPOLLIN */
            if (ncb_if_wblocked(ncb)) {
                ncb_cancel_wblock(ncb);
                iomod(ncb, EPOLLIN);
                nis_call_ecr("nshost.wpool.task:link %lld write block cancelled.", ncb->hld);
            }
            
            /* if complete write occurs during IO-isolation.
                Every task completed in this way which task type is kTaskType_TxOrder, it need to automatically trigger the next task check  */
            next = (struct task_node *)malloc(sizeof(struct task_node));
            if (next) {
                next->hld = task->hld;
                next->type = kTaskType_TxOrder;
                next->thread = task->thread;
                INIT_LIST_HEAD(&next->link);
                __add_task(next);
            }
        }
    }
    
    objdefr(hld);
    return retval;
}

static void *__run(void *p) {
    struct task_node *task;
    struct wthread *thread;
    int retval;
    
    thread = (struct wthread *)p;
    nis_call_ecr("nshost.wpool.init: LWP:%u startup.", posix__gettid());

    while (!__wpool.stop) {
        retval = posix__waitfor_waitable_handle(&thread->signal, 10);
        if ( retval < 0) {
            break;
        }

        /* reset wait object to block status immediately when the wait object timeout */
        if ( 0 == retval ) {
            posix__block_waitable_handle(&thread->signal);
        }

        /* complete all write task when once signal arrived,
            no matter which thread wake up this wait object */
        while ((NULL != (task = __get_task(thread)) ) && !__wpool.stop) {
            __run_task(task);
            free(task);
        }
    }

    nis_call_ecr("nshost.pool.wpool: LWP:%u terminated.", posix__gettid());
    pthread_exit((void *) 0);
    return NULL;
}

static int __wpool_init() {
    int i;
    
    __wpool.stop = posix__false;
    __wpool.wthread_count = get_nprocs();
    __wpool.write_threads = (struct wthread *)malloc(sizeof(struct wthread) * __wpool.wthread_count);
    if (!__wpool.write_threads) {
        return -ENOMEM;
    }
    
    for (i = 0; i < __wpool.wthread_count; i++){
        INIT_LIST_HEAD(&__wpool.write_threads[i].tasks);
        posix__init_notification_waitable_handle(&__wpool.write_threads[i].signal);
        posix__pthread_mutex_init(&__wpool.write_threads[i].mutex);
        __wpool.write_threads[i].task_list_size = 0;
        posix__pthread_create(&__wpool.write_threads[i].thread, &__run, (void *)&__wpool.write_threads[i]);
    }
    
    return 0;
}

posix__atomic_initial_declare_variable(__inited__);

int write_pool_init() {
    if (posix__atomic_initial_try(&__inited__)) {
        if (__wpool_init() < 0) {
            posix__atomic_initial_exception(&__inited__);
        } else {
            posix__atomic_initial_complete(&__inited__);
        }
    }

    return __inited__;
}

void write_pool_uninit(){
    int i;
    void *retval;
    struct task_node *task;
    
    if (!posix__atomic_initial_regress(__inited__)) {
        return;
    }
    
    __wpool.stop = posix__true;
    for (i = 0; i < __wpool.wthread_count; i++){
        posix__sig_waitable_handle(&__wpool.write_threads[i].signal);
        posix__pthread_join(&__wpool.write_threads[i].thread, &retval);
        
        /* 清理来不及处理的任务 */
        posix__pthread_mutex_lock(&__wpool.write_threads[i].mutex);
        while (NULL != (task = __get_task(&__wpool.write_threads[i]))) {
            free(task);
        }
        posix__pthread_mutex_unlock(&__wpool.write_threads[i].mutex);
        
        INIT_LIST_HEAD(&__wpool.write_threads[i].tasks);
        posix__uninit_waitable_handle(&__wpool.write_threads[i].signal);
        posix__pthread_mutex_uninit(&__wpool.write_threads[i].mutex);
    }
    
    free(__wpool.write_threads);
    __wpool.write_threads = NULL;
}

int post_write_task(objhld_t hld, enum task_type type) {
    struct task_node *task;

    if (!posix__atomic_initial_passed(__inited__)) {
        return -1;
    }
    
    if (NULL == (task = (struct task_node *)malloc(sizeof(struct task_node)))){
        return RE_ERROR(ENOMEM);
    }

    task->hld = hld;
    task->type = type;
    task->thread = &__wpool.write_threads[hld % __wpool.wthread_count];
    
    __add_task(task);
    posix__sig_waitable_handle(&task->thread->signal);
    return 0;
}