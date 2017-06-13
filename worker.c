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

struct task_node_t {
    objhld_t hld;
    enum task_type_t type;
    struct list_head link;
};

struct worker_thread_manager_t {
    posix__pthread_t *threads;
    posix__boolean_t stop;
    int thread_count;
    posix__pthread_mutex_t task_lock;
    posix__waitable_handle_t task_signal;
    struct list_head task_list; /* task_node_t::link */
    int task_list_size;
} ;

static int refcnt = 0;
extern uint64_t itime;
static struct worker_thread_manager_t task_thread_pool;

void add_task(struct task_node_t *task){
    if (task) {
        INIT_LIST_HEAD(&task->link);
        posix__pthread_mutex_lock(&task_thread_pool.task_lock);
        list_add_tail(&task->link, &task_thread_pool.task_list);
        ++task_thread_pool.task_list_size;
        posix__pthread_mutex_unlock(&task_thread_pool.task_lock);
    }
}

struct task_node_t *get_task(){
    struct task_node_t *task;
    
    posix__pthread_mutex_lock(&task_thread_pool.task_lock);
    if (NULL != (task = list_first_entry_or_null(&task_thread_pool.task_list, struct task_node_t, link))) {
         --task_thread_pool.task_list_size;
        list_del(&task->link);
        INIT_LIST_HEAD(&task->link);
    }
    posix__pthread_mutex_unlock(&task_thread_pool.task_lock);
    
    return task;
}

static int run_task(struct task_node_t *task) {
    ncb_t *ncb;
    int (*handler)(struct _ncb *);
    objhld_t hld;
    int retval;

    assert(NULL != task);

    hld = task->hld;
    handler = NULL;
    retval = -1;
    
    /* 对象已经不存在， 任务无法处理 */
    ncb = objrefr(hld);
    if (!ncb) {
        return -1;
    }

    do {
        if (task->type == kTaskType_Destroy) {
            ncb_report_debug_information(ncb, "destroy task from kernel.");
            objclos(hld);
            retval = 0;
            break;
        }
        
        /* 对象没有指定读写例程， 任务无法处理 */
        if (task->type == kTaskType_RxOrder || task->type == kTaskType_RxAttempt) {
            
            handler = ncb->ncb_read;

            /* 在任务初步检查过程中， 进行 Rx 操作的序列性校验 
             * 校验后如果认为需要因为保障序列性而放弃本线程的本次任务，则置空 handler*/
            if (ncb->proto_type == kProtocolType_TCP) {
                posix__pthread_mutex_lock(&ncb->rx_prot_lock);
                do {
                    if (!ncb->rx_running) {
                        ncb->rx_running = posix__true;
                        break;
                    }

                    /* 正在执行中，需要对来自 epoll 的任务做累加计数处理
                     * 并在后续过程中将其转换为 attempt 任务 */
                    if (task->type == kTaskType_RxOrder) {
                        ncb->rx_order_count++;
                        handler = NULL;
                        break;
                    }

                    /* 正在执行中， 收到尝试任务， 此时判断 order 计数是否为空
                     * 为空则放弃本次任务
                     * 不为空则允许执行, 同时递减 order 计数 
                     *
                     * 这个逻辑主要用于保证如下特殊情景:
                     * tcp_rx 运行中读出数据，将内核缓冲区读空，发起一次 attempt 任务后， 已经发现EAGAIN, 但是还没有解除"运行中"状态
                     * 此时 epoll 触发，并且该任务的线程得到优先调度， 很快的走到任务池中
                     * 如果仅仅凭tcp_rx 对状态的判断， 不足以保证不丢失数据
                     * 现行的策略则可以保证每个epoll触发都能在正常流程后增加一次 attempt */
                    if ((task->type == kTaskType_RxAttempt) && (ncb->rx_order_count > 0)) {
                        --ncb->rx_order_count;
                        break;
                    }

                    /* 没有任何 order 计数的 attempt 任务，说明该任务来自于 Rx 过程的自检
                     * Rx 过程要求读到 EAGAIN 为止, 只要没有发生 EAGAIN, Rx 过程都会触发继续自检 */
                    if ((task->type == kTaskType_RxAttempt) && (ncb->rx_order_count == 0)) {
                        break;
                    }
                } while (0);
                posix__pthread_mutex_unlock(&ncb->rx_prot_lock);
            }
        } else if (task->type == kTaskType_TxOrder) {
            handler = ncb->ncb_write;
        } else {
            break;
        }

        if (!handler) {
            break;
        }
        
        /*
         * 重新明确公用处理例程返回值定义
         * 1. Rx:
         * 2. Tx:
         *  2.1 retval < 0 表示过程内部发生系统调用错误等无法容忍且无法处理的错误 
         * 2.2 retval == 0 调用成功， 读/写操作按照既定字节量完成
         * 2.3 retval > 0(EAGAIN) (通常是写操作导致的)无法立即完成，任务需要重入       
         */
        retval = handler(ncb);
        if (retval/*EAGAIN*/ > 0) {
            add_task(task);
        } else if (retval < 0) {
            objclos(ncb->hld_);
        } else /*0 == retval*/ {
            ;
        }
    } while (0);

    objdefr(hld);
    return retval;
}

static void *run(void *p) {
    struct task_node_t *task;


    while (!task_thread_pool.stop) {

        if (posix__waitfor_waitable_handle(&task_thread_pool.task_signal, -1) < 0) {
            break;
        }

        /* 如果发生IO阻止(返回值大于0)， 则任务保留
             * 否则任务均销毁 */
        while (NULL != (task = get_task())) {
            if (run_task(task) <= 0) {
                free(task);
            }
        }
    }

    pthread_exit((void *) 0);
    return NULL;
}

int wtpinit() {
    int i;

    if (posix__atomic_inc(&refcnt) > 1) {
        return 0;
    }

    /* 核心数量 * 2 +2 是什么鬼 */
    task_thread_pool.task_list_size = 0;
    task_thread_pool.thread_count = get_nprocs() * 2;// + 2;

    if (NULL == (task_thread_pool.threads = (posix__pthread_t *)malloc(task_thread_pool.thread_count * sizeof(posix__pthread_t)))){
        return -1;
    }
    
    INIT_LIST_HEAD(&task_thread_pool.task_list);
    posix__init_synchronous_waitable_handle(&task_thread_pool.task_signal);
    posix__pthread_mutex_init(&task_thread_pool.task_lock);
    
    task_thread_pool.stop = posix__false;
    for (i = 0; i < task_thread_pool.thread_count; i++){
        posix__pthread_create(&task_thread_pool.threads[i], &run, NULL);
    }
    return 0;
}

void wtpuninit() {
    int i;

    if (0 == refcnt) {
        return;
    }

    /* 使用引用计数，保证所有调用过 INIT 的使用者均已认可反初始化 */
    if (0 != posix__atomic_dec(&refcnt)) {
        return;
    }
    
    if (task_thread_pool.threads && task_thread_pool.thread_count > 0){
        task_thread_pool.stop = posix__true;
        for (i = 0; i < task_thread_pool.thread_count; i++){
            void *retptr;
            posix__pthread_join(&task_thread_pool.threads[i], &retptr);
        }
        free(task_thread_pool.threads);
        task_thread_pool.threads = NULL;
        task_thread_pool.thread_count = 0;
    }
    
    INIT_LIST_HEAD(&task_thread_pool.task_list);
    posix__uninit_waitable_handle(&task_thread_pool.task_signal);
    posix__pthread_mutex_uninit(&task_thread_pool.task_lock);
}

int post_task(objhld_t hld, enum task_type_t ttype) {
    struct task_node_t *task;

    if (hld < 0) {
        return -EINVAL;
    }
    
    /* 分配任务， 指定任务操作的 NCB 对象 */
    if (NULL == (task = (struct task_node_t *) malloc(sizeof (struct task_node_t)))) {
        return -1;
    }
    task->type = ttype;
    task->hld = hld;
    add_task(task);
    
    /* 通知等待中的线程, 可以执行 */
    posix__sig_waitable_handle(&task_thread_pool.task_signal);
    return 0;
}