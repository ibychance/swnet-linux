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

struct read_pool_manager {
    posix__pthread_t *threads;
    posix__boolean_t stop;
    int thread_count;
    posix__pthread_mutex_t task_lock;
    posix__waitable_handle_t task_signal;
    struct list_head task_list; /* task_node_t::link */
    int task_list_size;
} ;

static int refcnt = 0;
static struct read_pool_manager read_pool;

static void add_task(struct task_node_t *task){
    if (task) {
        INIT_LIST_HEAD(&task->link);
        posix__pthread_mutex_lock(&read_pool.task_lock);
        list_add_tail(&task->link, &read_pool.task_list);
        ++read_pool.task_list_size;
        posix__pthread_mutex_unlock(&read_pool.task_lock);
    }
}

static struct task_node_t *get_task(){
    struct task_node_t *task;
    
    posix__pthread_mutex_lock(&read_pool.task_lock);
    if (NULL != (task = list_first_entry_or_null(&read_pool.task_list, struct task_node_t, link))) {
         --read_pool.task_list_size;
        list_del(&task->link);
        INIT_LIST_HEAD(&task->link);
    }
    posix__pthread_mutex_unlock(&read_pool.task_lock);
    
    return task;
}

static int run_task(struct task_node_t *task) {
    ncb_t *ncb;
    objhld_t hld;
    int retval;

    assert(NULL != task);

    hld = task->hld;
    
    /* 处理关闭事件 */
    if (task->type == kTaskType_Destroy) {
        objclos(hld);
        return 0;
    }
    
    retval = -1;
    
    /* 对象已经不存在， 任务无法处理 */
    ncb = objrefr(hld);
    if (!ncb) {
        return -1;
    }

    /* 在任务初步检查过程中， 进行 Rx 操作的序列性校验 
     * 校验后如果认为需要因为保障序列性而放弃本线程的本次任务，则执行任务阻止*/
    if (ncb->proto_type == kProtocolType_TCP) {
        posix__pthread_mutex_lock(&ncb->rx_prot_lock);
        do {
            if (!ncb->rx_running) {
                ncb->rx_running = posix__true;
                break;
            }

            /* 正在执行中， 收到尝试任务， 此时判断 order 计数是否为空
             * 为空则放弃本次任务
             * 不为空则允许执行, 同时递减 order 计数 
             *
             * 这个逻辑主要用于保证如下特殊情景:
             * tcp_rx 运行中读出数据，将内核缓冲区读空，发起一次 RxTest 任务后， 已经发现EAGAIN, 但是还没有解除"运行中"状态
             * 此时 epoll 触发，并且该任务的线程得到优先调度， 很快的走到任务池中
             * 如果仅仅凭tcp_rx 对状态的判断， 不足以保证不丢失数据
             * 现行的策略则可以保证每个epoll触发都能在正常流程后增加一次 RxTest */
            if ((task->type == kTaskType_RxTest) && (ncb->rx_order_count > 0)) {
                //printf("read decrement.\n");
                --ncb->rx_order_count;
                break;
            }

            /* 没有任何 order 计数的 RxTest 任务，说明该任务来自于 Rx 过程的自检
             * Rx 过程要求读到 EAGAIN 为止, 只要没有发生 EAGAIN, Rx 过程都会触发继续自检 */
            if ((task->type == kTaskType_RxTest) && (ncb->rx_order_count == 0)) {
                break;
            }
            
            /* 正在执行中，需要对来自 epoll 的任务做累加计数处理
             * 并在后续过程中将其转换为 RxTest 任务, 但本次任务将被阻止 */
            if (task->type == kTaskType_RxOrder) {
                //printf("read increment.\n");
                ncb->rx_order_count++;
            }
            objdefr(hld);
            posix__pthread_mutex_unlock(&ncb->rx_prot_lock);
            return 0;
        } while (0);
        posix__pthread_mutex_unlock(&ncb->rx_prot_lock);
    }

    //printf("[%u]befor ncb read %llu\n", posix__gettid(), posix__clock_gettime());
    retval = ncb->ncb_read(ncb);
    
    /* 本轮顺利从内核缓冲区读出数据，复用任务，发起自检
     * 为了防止任何一个链接饿死, 这里对任何链接都不作recv完的处理, 而是采用追加任务的方法 */
    if (0 == retval){
        //printf("[%u] read ok %llu\n", posix__gettid(), posix__clock_gettime());
        task->type = kTaskType_RxTest;
        add_task(task);
    }
    
    /* 发生系统调用错误等， 无法恢复的内部错误， 链接将被关闭 */
    else if (retval < 0){
        objclos(ncb->hld);
    }
    
    /* EAGAIN  */
    else if (retval > 0){
        //printf("[%u]read EAGAIN %llu\n", posix__gettid(), posix__clock_gettime());
    }

    objdefr(hld);
    return retval;
}

static void *run(void *p) {
    struct task_node_t *task;

    while (!read_pool.stop) {

        if (posix__waitfor_waitable_handle(&read_pool.task_signal, -1) < 0) {
            break;
        }

        /* 如果发生IO阻止(返回值大于0)， 则任务保留
             * 否则任务均销毁 */
        while (NULL != (task = get_task())) {
            if (0 != run_task(task)) {
                free(task);
            }
        }
    }

    pthread_exit((void *) 0);
    return NULL;
}

int read_pool_init() {
    int i;

    if (posix__atomic_inc(&refcnt) > 1) {
        return 0;
    }

    /* 核心数量 * 2 +2 是什么鬼 */
    read_pool.task_list_size = 0;
    read_pool.thread_count = get_nprocs();

    if (NULL == (read_pool.threads = (posix__pthread_t *)malloc(read_pool.thread_count * sizeof(posix__pthread_t)))){
        return -1;
    }
    
    INIT_LIST_HEAD(&read_pool.task_list);
    posix__init_synchronous_waitable_handle(&read_pool.task_signal);
    posix__pthread_mutex_init(&read_pool.task_lock);
    
    read_pool.stop = posix__false;
    for (i = 0; i < read_pool.thread_count; i++){
        posix__pthread_create(&read_pool.threads[i], &run, NULL);
    }
    return 0;
}

void read_pool_uninit() {
    int i;
    struct task_node_t *task;
    
    if (0 == refcnt) {
        return;
    }

    /* 使用引用计数，保证所有调用过 INIT 的使用者均已认可反初始化 */
    if (0 != posix__atomic_dec(&refcnt)) {
        return;
    }
    
    if (read_pool.threads && read_pool.thread_count > 0){
        read_pool.stop = posix__true;
        for (i = 0; i < read_pool.thread_count; i++){
            void *retptr;
            posix__pthread_join(&read_pool.threads[i], &retptr);
        }
        free(read_pool.threads);
        read_pool.threads = NULL;
        read_pool.thread_count = 0;
    }

    /* 清理来不及处理的任务 */
    posix__pthread_mutex_lock(&read_pool.task_lock);
    while (NULL != (task = get_task())) {
        free(task);
    }
    posix__pthread_mutex_unlock(&read_pool.task_lock);
    
    INIT_LIST_HEAD(&read_pool.task_list);
    posix__uninit_waitable_handle(&read_pool.task_signal);
    posix__pthread_mutex_uninit(&read_pool.task_lock);
}

int post_read_task(objhld_t hld, enum task_type_t type) {
    struct task_node_t *task;

    if (hld < 0) {
        return -EINVAL;
    }
    
    /* 分配任务， 指定任务操作的 NCB 对象 */
    if (NULL == (task = (struct task_node_t *) malloc(sizeof (struct task_node_t)))) {
        return -1;
    }
    task->type = type;
    task->hld = hld;

    /* 通知等待中的线程, 可以执行 */
//    posix__pthread_mutex_lock(&read_pool.task_lock);
//    list_add_tail(&task->link, &read_pool.task_list);
//    ++read_pool.task_list_size;
//    posix__pthread_mutex_unlock(&read_pool.task_lock);
    
    add_task(task);
    posix__sig_waitable_handle(&read_pool.task_signal);
    return 0;
}