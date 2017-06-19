#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/sysinfo.h>

#include "object.h"
#include "clist.h"
#include "ncb.h"
#include "worker.h"
#include "tcp.h"

#include "posix_thread.h"
#include "posix_wait.h"
#include "posix_atomic.h"
#include "posix_types.h"
#include "posix_ifos.h"

struct write_thread_node {
    posix__pthread_t    thread;
    posix__pthread_mutex_t task_lock;
    posix__waitable_handle_t task_signal;
    struct list_head task_list; /* task_node_t::link */
    int task_list_size;
};

struct write_pool_manager {
    struct write_thread_node *write_threads;
    int write_thread_count;
    posix__boolean_t    stop;
};

static int refcnt = 0;
static struct write_pool_manager write_pool;

static void add_task(struct write_thread_node *thread, struct task_node_t *task) {
    if (task && thread) {
        INIT_LIST_HEAD(&task->link);
        posix__pthread_mutex_lock(&thread->task_lock);
        list_add_tail(&task->link, &thread->task_list);
        ++thread->task_list_size;
        posix__pthread_mutex_unlock(&thread->task_lock);
    }
}

static struct task_node_t *get_task(struct write_thread_node *thread){
    struct task_node_t *task;
    
    posix__pthread_mutex_lock(&thread->task_lock);
    if (NULL != (task = list_first_entry_or_null(&thread->task_list, struct task_node_t, link))) {
         --thread->task_list_size;
        list_del(&task->link);
        INIT_LIST_HEAD(&task->link);
    }
    posix__pthread_mutex_unlock(&thread->task_lock);
    
    return task;
}

static int run_task(struct write_thread_node *thread, struct task_node_t *task) {
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
            iomod(ncb, kPollMask_Read);
        }
    }
    
    /* 
     * 发生 EAGAIN， 并且已经设置了IO隔离，因为写操作对象总是在同一个线程上下文进行，因此不存在线程安全问题
     * 设置IO隔离， 同时将EPOLL设置为关注写入缓冲区
     */
    else if (EAGAIN == retval ){
        ncb_mark_wblocked(ncb);
        iomod(ncb, kPollMask_Read | kPollMask_Write);
    }
    
    /* 没有任何数据需要写入， 本轮轮空 */
    else {
        ;
    }
    
    objdefr(hld);
    return retval;
}

static void *run(void *p) {
    struct task_node_t *task;
    struct write_thread_node *thread;
    
    thread = (struct write_thread_node *)p;
    while (!write_pool.stop) {

        if (posix__waitfor_waitable_handle(&thread->task_signal, -1) < 0) {
            break;
        }

        /* 过程返回 0. 则要求保留 task 内存 */
        while (NULL != (task = get_task(thread))) {
            if (0 != run_task(thread, task)) {
                free(task);
            }
        }
    }

    pthread_exit((void *) 0);
    return NULL;
}

int write_pool_init(){
    int i;
    
    if (posix__atomic_inc(&refcnt) > 1) {
        return 0;
    }
    
    write_pool.stop = posix__false;
    write_pool.write_thread_count = get_nprocs();
    write_pool.write_threads = (struct write_thread_node *)malloc(sizeof(struct write_thread_node) * write_pool.write_thread_count);
    if (!write_pool.write_threads) {
        return -1;
    }
    
    for (i = 0; i < write_pool.write_thread_count; i++){
        INIT_LIST_HEAD(&write_pool.write_threads[i].task_list);
        posix__init_synchronous_waitable_handle(&write_pool.write_threads[i].task_signal);
        posix__pthread_mutex_init(&write_pool.write_threads[i].task_lock);
        write_pool.write_threads[i].task_list_size = 0;
        posix__pthread_create(&write_pool.write_threads[i].thread, &run, (void *)&write_pool.write_threads[i]);
    }
    
    return 0;
}

void write_pool_uninit(){
    int i;
    void *retval;
    struct task_node_t *task;
    
    if (0 == refcnt) {
        return;
    }

    /* 使用引用计数，保证所有调用过 INIT 的使用者均已认可反初始化 */
    if (0 != posix__atomic_dec(&refcnt)) {
        return;
    }
    
    write_pool.stop = posix__true;
    for (i = 0; i < write_pool.write_thread_count; i++){
        posix__pthread_join(&write_pool.write_threads[i].thread, &retval);
        
        /* 清理来不及处理的任务 */
        posix__pthread_mutex_lock(&write_pool.write_threads[i].task_lock);
        while (NULL != (task = get_task(&write_pool.write_threads[i]))) {
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

int post_write_task(objhld_t hld, enum task_type_t type){
    struct task_node_t *task;
    struct write_thread_node *thread;
    
    if (NULL == (task = (struct task_node_t *)malloc(sizeof(struct task_node_t)))){
        return -1;
    }
    task->hld = hld;
    task->type = type;
    
    thread = &write_pool.write_threads[hld % write_pool.write_thread_count];
    
//    posix__pthread_mutex_lock(&thread->task_lock);
//    list_add_tail(&task->link, &thread->task_list);
//    ++thread->task_list_size;
//    posix__pthread_mutex_unlock(&thread->task_lock);
    
    add_task(thread, task);
    posix__sig_waitable_handle(&thread->task_signal);
    return 0;
}