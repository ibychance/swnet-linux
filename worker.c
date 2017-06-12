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

typedef struct {
    objhld_t hld_;
    enum task_type_t ttype_;
    struct list_head link_;
} task_node_t;

typedef struct {
    posix__pthread_t thread_;
    posix__pthread_mutex_t task_lock_;
    posix__waitable_handle_t waiter_;
    struct list_head task_head_; /* task_node_t */
    posix__boolean_t actived_; /* 线程内部用于判断是否退出， 线程外部用于判断线程是否正常*/
} pthread_node_t;

struct {
    int recv_thcnt_;
    pthread_node_t *recv_;
    int send_thcnt_;
    pthread_node_t *send_;
} pthread_manager = {0, NULL, 0, NULL};

struct {
    int thcnt_;
    posix__pthread_t *parser_;
    struct list_head task_head_; /* task_node_t */
    posix__pthread_mutex_t task_lock_;
    posix__waitable_handle_t waiter_;
    posix__boolean_t stop_;
}pthread_parser;

static int refcnt = 0;

static int run_task(task_node_t *task, pthread_node_t *pthread_node) {
    ncb_t *ncb;
    int (*handler)(struct _ncb *);
    objhld_t hld;
    int retval;

    assert(NULL != task);

    hld = task->hld_;
    handler = NULL;
    retval = -1;
    
    /* 对象已经不存在， 任务无法处理 */
    ncb = objrefr(hld);
    if (!ncb) {
        return -1;
    }

    do {
        if (task->ttype_ == kTaskType_Destroy) {
            ncb_report_debug_information(ncb, "destroy task from kernel.");
            objclos(hld);
            retval = 0;
            break;
        }
        
        /* 对象没有指定读写例程， 任务无法处理 */
        if (task->ttype_ == kTaskType_Read) {
            handler = ncb->on_read_;
        } else if (task->ttype_ == kTaskType_Write) {
            handler = ncb->on_write_;
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
            INIT_LIST_HEAD(&task->link_);
            posix__pthread_mutex_lock(&pthread_node->task_lock_);
            list_add_tail(&task->link_, &pthread_node->task_head_);
            posix__pthread_mutex_unlock(&pthread_node->task_lock_);
        } else if (retval < 0) {
            objclos(ncb->hld_);
        } else /*0 == retval*/ {
            ;
        }
    } while (0);

    objdefr(hld);
    return retval;
}

static void *user(void *p){
    task_node_t *task;
    ncb_t *ncb;
    int retval;
    
    while (!pthread_parser.stop_){
        if (posix__waitfor_waitable_handle(&pthread_parser.waiter_, -1) < 0) {
            break;
        }
        
        while (1) {
            posix__pthread_mutex_lock(&pthread_parser.task_lock_);
            if (NULL == (task = list_first_entry_or_null(&pthread_parser.task_head_, task_node_t, link_))) {
                posix__pthread_mutex_unlock(&pthread_parser.task_lock_);
                break;
            }
            list_del(&task->link_);
            posix__pthread_mutex_unlock(&pthread_parser.task_lock_);
            
            retval = -1;
            
            ncb = objrefr(task->hld_);
            if (ncb) {
                retval = ncb->on_userio_(ncb);
                objdefr(task->hld_);
            }
            
            if (retval <= 0) {
                free(task);
            }else{
                /* task 复用 */
                posix__pthread_mutex_lock(&pthread_parser.task_lock_);
                INIT_LIST_HEAD(&task->link_);
                list_add_tail(&task->link_,&pthread_parser.task_head_ );
                posix__pthread_mutex_unlock(&pthread_parser.task_lock_);
            }
        }
    }
    
    pthread_exit((void *) 0);
    return NULL;
}

static void *run(void *p) {
    pthread_node_t *pthread_node;
    task_node_t *task;

    pthread_node = (pthread_node_t *) p;

    while (pthread_node->actived_) {

        if (posix__waitfor_waitable_handle(&pthread_node->waiter_, -1) < 0) {
            break;
        }

        while (1) {
            posix__pthread_mutex_lock(&pthread_node->task_lock_);
            if (NULL == (task = list_first_entry_or_null(&pthread_node->task_head_, task_node_t, link_))) {
                posix__pthread_mutex_unlock(&pthread_node->task_lock_);
                break;
            }
            list_del(&task->link_);
            posix__pthread_mutex_unlock(&pthread_node->task_lock_);

            /* 如果发生IO阻止(返回值大于0)， 则任务保留 */
            if (run_task(task, p) <= 0) {
                free(task);
            }
        }
    }

    pthread_exit((void *) 0);
    return NULL;
}

static int init_pthread_node(pthread_node_t *pthread_node) {
    if (!pthread_node) {
        return -1;
    }

    pthread_node->actived_ = posix__false;

    if (posix__init_synchronous_waitable_handle(&pthread_node->waiter_) < 0) {
        return -1;
    }

    posix__pthread_mutex_init(&pthread_node->task_lock_);
    INIT_LIST_HEAD(&pthread_node->task_head_);

    pthread_node->actived_ = posix__true;
    if (posix__pthread_create(&pthread_node->thread_, &run, pthread_node) < 0) {
        posix__uninit_waitable_handle(&pthread_node->waiter_);
        posix__pthread_mutex_release(&pthread_node->task_lock_);
        pthread_node->actived_ = posix__false;
        return -1;
    }
    return 0;
}

static void uninit_pthread_node(pthread_node_t *pthread_node) {
    task_node_t *task;
    void *thret;

    if (!pthread_node) {
        return;
    }

    if (!pthread_node->actived_) {
        return;
    }

    /* 停止工作线程, 释放等待对象 */
    pthread_node->actived_ = posix__false;
    posix__sig_waitable_handle(&pthread_node->waiter_);
    posix__pthread_join(&pthread_node->thread_, &thret);
    posix__uninit_waitable_handle(&pthread_node->waiter_);

    /* 处理线程退出后，尚未处理的参与内存, 还原链表为初始状态 */
    posix__pthread_mutex_lock(&pthread_node->task_lock_);
    while (NULL != (task = list_first_entry_or_null(&pthread_node->task_head_, task_node_t, link_))) {
        list_del(&task->link_);
        free(task);
    }
    INIT_LIST_HEAD(&pthread_node->task_head_);
    posix__pthread_mutex_unlock(&pthread_node->task_lock_);
    posix__pthread_mutex_release(&pthread_node->task_lock_);
}

int pthread_parser_init(){
    int i;
    
    /* 核心数量 * 2 +2 是什么鬼 */
    pthread_parser.thcnt_ = 2 * get_nprocs() + 2;
    pthread_parser.parser_ = (posix__pthread_t *)malloc(pthread_parser.thcnt_ * sizeof(posix__pthread_t));
    if (!pthread_parser.parser_){
        return -1;
    }
    
    INIT_LIST_HEAD(&pthread_parser.task_head_);
    posix__pthread_mutex_init(&pthread_parser.task_lock_);
    posix__init_synchronous_waitable_handle(&pthread_parser.waiter_);
    
    for (i = 0; i <pthread_parser.thcnt_; i++ ){
        if (posix__pthread_create(&pthread_parser.parser_[i], &user, NULL) < 0) {
            posix__uninit_waitable_handle(&pthread_parser.waiter_);
            posix__pthread_mutex_release(&pthread_parser.task_lock_);
            return -1;
        }
    }
    return 0;
}

int pthread_manager_init(int rthcnt, int wthcnt) {
    int nothcnt;
    int arthcnt, awthcnt;
    int i;

    if (posix__atomic_inc(&refcnt) > 1) {
        return 0;
    }

    /* 核心数量 * 2 +2 是什么鬼 */
    nothcnt = get_nprocs();

    arthcnt = ((rthcnt <= 0) ? nothcnt : rthcnt);
    awthcnt = ((wthcnt <= 0) ? nothcnt : wthcnt);

    pthread_manager.recv_thcnt_ = arthcnt;
    pthread_manager.send_thcnt_ = awthcnt;
    
    if (NULL == (pthread_manager.recv_ = (pthread_node_t *) malloc(sizeof (pthread_node_t) * pthread_manager.recv_thcnt_))) {
        return -1;
    }
    if (NULL == (pthread_manager.send_ = (pthread_node_t *) malloc(sizeof (pthread_node_t) * pthread_manager.send_thcnt_))) {
        return -1;
    }
    
    for (i = 0; i < pthread_manager.recv_thcnt_; i++) {
        init_pthread_node(&pthread_manager.recv_[i]);
    }

    for (i = 0; i < pthread_manager.send_thcnt_; i++) {
        init_pthread_node(&pthread_manager.send_[i]);
    }

    pthread_parser_init();
    return 0;
}

void pthread_manager_uninit() {
    int i;

    if (0 == refcnt) {
        return;
    }

    /* 使用引用计数，保证所有调用过 INIT 的使用者均已认可反初始化 */
    if (0 != posix__atomic_dec(&refcnt)) {
        return;
    }

    if (pthread_manager.recv_thcnt_ > 0 && pthread_manager.recv_) {
        for (i = 0; i < pthread_manager.recv_thcnt_; i++) {
            uninit_pthread_node(&pthread_manager.recv_[i]);
        }
        free(pthread_manager.recv_);
        pthread_manager.recv_ = NULL;
        pthread_manager.recv_thcnt_ = 0;
    }


    if (pthread_manager.send_thcnt_ > 0 && pthread_manager.send_) {
        for (i = 0; i < pthread_manager.send_thcnt_; i++) {
            uninit_pthread_node(&pthread_manager.send_[i]);
        }
        free(pthread_manager.send_);
        pthread_manager.send_ = NULL;
        pthread_manager.send_thcnt_ = 0;
    }
}

static pthread_node_t *select_worker(objhld_t hld, pthread_node_t *pthread_node_set, int pthread_node_count) {
    pthread_node_t *pthread_node_selected;
    int i;

    if (!pthread_node_set || pthread_node_count <= 0 || hld < 0) {
        return NULL;
    }

    /* 简单使用句柄字面值哈希到线程集合 */
    if (NULL == (pthread_node_selected = &pthread_node_set[hld % pthread_node_count])) {
        return NULL;
    }

    /* 如果哈希所得线程是活跃状态， 则可以直接使用 */
    if (pthread_node_selected->actived_) {
        return pthread_node_selected;
    }

    /* 指定线程不处于活跃状态, 则统筹到第一个已激活线程 */
    for (i = 0; i < pthread_node_count; i++) {
        if (pthread_node_set[i].actived_ >= 0) {
            return &pthread_node_set[i];
        }
    }
    return NULL;
}

int post_task(objhld_t hld, enum task_type_t ttype) {
    task_node_t *task;
    pthread_node_t *pthread_node;

    if (hld < 0) {
        return -EINVAL;
    }

    pthread_node = NULL;
    
    /* 线程调度方式， 采用对象固定线程
     * IO 对象拥有自己的一个读线程和一个写线程
     * 但任何一个读/写线程都可能被多个IO对象拥有
     *  */
    if (kTaskType_Read == ttype || kTaskType_Destroy == ttype) {
        pthread_node = select_worker(hld, pthread_manager.recv_, pthread_manager.recv_thcnt_);
    } else if (kTaskType_Write == ttype) {
        pthread_node = select_worker(hld, pthread_manager.send_, pthread_manager.send_thcnt_);
    }
    
    /* 分配任务， 指定任务操作的 NCB 对象 */
    if (NULL == (task = (task_node_t *) malloc(sizeof (task_node_t)))) {
        return -1;
    }
    task->ttype_ = ttype;
    task->hld_ = hld;
    
    if (kTaskType_User == ttype){
        posix__pthread_mutex_lock(&pthread_parser.task_lock_);
        list_add_tail(&task->link_, &pthread_parser.task_head_);
        posix__pthread_mutex_unlock(&pthread_parser.task_lock_);
        posix__sig_waitable_handle(&pthread_parser.waiter_);
        return 0;
    }

    if (pthread_node) {
        /* IO 对象插入线程的任务列表 */
        posix__pthread_mutex_lock(&pthread_node->task_lock_);
        list_add_tail(&task->link_, &pthread_node->task_head_);
        posix__pthread_mutex_unlock(&pthread_node->task_lock_);

        /* 通知等待中的线程, 可以执行 */
        posix__sig_waitable_handle(&pthread_node->waiter_);
        return 0;
    }
    
    free(task);
    return -1;
}