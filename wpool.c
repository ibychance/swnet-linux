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
#include "worker.h"
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
struct task_node {
    objhld_t hld;
    enum task_type type;
    struct list_head link;
};

struct write_thread_node {
    posix__pthread_t    thread;
    posix__pthread_mutex_t task_lock;
    posix__waitable_handle_t task_signal;
    struct list_head task_list; /* struct task_node::link */
    int task_list_size;
};

struct write_pool_manager {
    struct write_thread_node *write_threads;
    int write_thread_count;
    posix__boolean_t    stop;
};
static struct write_pool_manager write_pool;

static void __add_task(struct write_thread_node *thread, struct task_node *task) {
    if (task && thread) {
        INIT_LIST_HEAD(&task->link);
        posix__pthread_mutex_lock(&thread->task_lock);
        list_add_tail(&task->link, &thread->task_list);
        ++thread->task_list_size;
        posix__pthread_mutex_unlock(&thread->task_lock);
    }
}

static struct task_node *__get_task(struct write_thread_node *thread){
    struct task_node *task;
    
    posix__pthread_mutex_lock(&thread->task_lock);
    if (NULL != (task = list_first_entry_or_null(&thread->task_list, struct task_node, link))) {
         --thread->task_list_size;
        list_del(&task->link);
        INIT_LIST_HEAD(&task->link);
    }
    posix__pthread_mutex_unlock(&thread->task_lock);
    
    return task;
}

static int __run_task(struct task_node *task) {
    objhld_t hld;
    int retval;
    ncb_t *ncb;

    assert(NULL != task);
    
    hld = task->hld;
    
    ncb = objrefr(hld);
    if (!ncb) {
        return -1;
    }
    
    /* 当前状态为IO隔离，则不能响应任何的 TxTest 任务 */
    if (ncb_if_wblocked(ncb)){
        if (task->type == kTaskType_TxTest) {
            objdefr(hld);
            return EAGAIN;
        }
    }
    
    /*
     * 代码走到此处，只能有两种情况
     * 1. 没有发生IO隔离
     * 2. 发生了IO隔离，但是收到了来自 EPOLL 的 TxOrder, 可以正常响应
     */
    assert (ncb->ncb_write);
    if (!ncb->ncb_write) {
        return -1;
    }
    retval = ncb->ncb_write(ncb);
    
    /* 发生无可挽救的系统错误, 该链接将被关闭 */
    if(retval < 0){
        objclos(ncb->hld);
    }
    
    /* 本次节点顺利写入内核  */
    else if (0 == retval) {
        /* 如果是在IO隔离期间发生的完成写入，则取消IO隔离，同时切换EPOLL关注读出缓冲区
         */
        if (task->type == kTaskType_TxOrder && ncb_if_wblocked(ncb) ) {
            ncb_cancel_wblock(ncb);
            iomod(ncb, EPOLLIN);
        }
    }
    
    /* 
     * 发生 EAGAIN， 并且已经设置了IO隔离，因为写操作对象总是在同一个线程上下文进行，因此不存在线程安全问题
     * 设置IO隔离， 同时将EPOLL设置为关注写入缓冲区
     */
    else if (EAGAIN == retval ){
        ncb_mark_wblocked(ncb);
        iomod(ncb, EPOLLIN | EPOLLOUT);
    }
    
    /* 没有任何数据需要写入， 本轮轮空 */
    else {
        ;
    }
    
    objdefr(hld);
    return retval;
}

static void *__run(void *p) {
    struct task_node *task;
    struct write_thread_node *thread;
    int retval;
    
    thread = (struct write_thread_node *)p;
    nis_call_ecr("nshost.pool.LWP:%u startup.", posix__gettid());

    while (!write_pool.stop) {
        retval = posix__waitfor_waitable_handle(&thread->task_signal, 10);
        if ( retval < 0) {
            break;
        }

        if ( 0 == retval ) {
            /* reset wait object to block status immediately */
            posix__block_waitable_handle(&thread->task_signal);
        }

        /* complete all write task when once signal arrived,
            no matter which thread wake up this wait object */
        while ((NULL != (task = __get_task(thread)) ) && !write_pool.stop) {
            __run_task(task);
            free(task);
        }
    }

    nis_call_ecr("nshost.pool.LWP:%u terminated.", posix__gettid());
    pthread_exit((void *) 0);
    return NULL;
}

static int __inited = -1;
static pthread_mutex_t __init_locker = PTHREAD_MUTEX_INITIALIZER;

int __write_pool_init() {
    int i;
    
    write_pool.stop = posix__false;
    write_pool.write_thread_count = get_nprocs();
    write_pool.write_threads = (struct write_thread_node *)malloc(sizeof(struct write_thread_node) * write_pool.write_thread_count);
    if (!write_pool.write_threads) {
        return RE_ERROR(ENOMEM);
    }
    
    for (i = 0; i < write_pool.write_thread_count; i++){
        INIT_LIST_HEAD(&write_pool.write_threads[i].task_list);
        posix__init_notification_waitable_handle(&write_pool.write_threads[i].task_signal);
        posix__pthread_mutex_init(&write_pool.write_threads[i].task_lock);
        write_pool.write_threads[i].task_list_size = 0;
        posix__pthread_create(&write_pool.write_threads[i].thread, &__run, (void *)&write_pool.write_threads[i]);
    }
    
    return 0;
}

int write_pool_init(){
    int retval;

    retval = 0;

    if (__inited >= 0) {
        return 0;
    }

    pthread_mutex_lock(&__init_locker);
     /* double check if other thread complete the initialize request */
    if (__inited < 0) {
        retval = __write_pool_init();
        if (retval < 0) {
            ;
        }else{
            posix__atomic_xchange(&__inited, retval);
        }
    }
    pthread_mutex_unlock(&__init_locker);
    return retval;
}

void write_pool_uninit(){
    int i;
    void *retval;
    struct task_node *task;
    
    if ( posix__atomic_xchange(&__inited, -1) < 0 ) {
        return;
    }
    
    write_pool.stop = posix__true;
    for (i = 0; i < write_pool.write_thread_count; i++){
        posix__sig_waitable_handle(&write_pool.write_threads[i].task_signal);
        posix__pthread_join(&write_pool.write_threads[i].thread, &retval);
        
        /* 清理来不及处理的任务 */
        posix__pthread_mutex_lock(&write_pool.write_threads[i].task_lock);
        while (NULL != (task = __get_task(&write_pool.write_threads[i]))) {
            free(task);
        }
        posix__pthread_mutex_unlock(&write_pool.write_threads[i].task_lock);
        
        INIT_LIST_HEAD(&write_pool.write_threads[i].task_list);
        posix__uninit_waitable_handle(&write_pool.write_threads[i].task_signal);
        posix__pthread_mutex_uninit(&write_pool.write_threads[i].task_lock);
    }
    
    free(write_pool.write_threads);
    write_pool.write_threads = NULL;
}

int post_write_task(objhld_t hld, enum task_type type){
    struct task_node *task;
    struct write_thread_node *thread;
    
    if (NULL == (task = (struct task_node *)malloc(sizeof(struct task_node)))){
        return RE_ERROR(ENOMEM);
    }

    task->hld = hld;
    task->type = type;
    
    thread = &write_pool.write_threads[hld % write_pool.write_thread_count];
    
    __add_task(thread, task);
    posix__sig_waitable_handle(&thread->task_signal);
    return 0;
}