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
    posix__boolean_t actived_; /* �߳��ڲ������ж��Ƿ��˳��� �߳��ⲿ�����ж��߳��Ƿ�����*/
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
    
    /* �����Ѿ������ڣ� �����޷����� */
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
        
        /* ����û��ָ����д���̣� �����޷����� */
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
         * ������ȷ���ô������̷���ֵ����
         * 1. Rx:
         * 2. Tx:
         *  2.1 retval < 0 ��ʾ�����ڲ�����ϵͳ���ô�����޷��������޷�����Ĵ��� 
         * 2.2 retval == 0 ���óɹ��� ��/д�������ռȶ��ֽ������
         * 2.3 retval > 0(EAGAIN) (ͨ����д�������µ�)�޷�������ɣ�������Ҫ����       
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
                /* task ���� */
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

            /* �������IO��ֹ(����ֵ����0)�� �������� */
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

    /* ֹͣ�����߳�, �ͷŵȴ����� */
    pthread_node->actived_ = posix__false;
    posix__sig_waitable_handle(&pthread_node->waiter_);
    posix__pthread_join(&pthread_node->thread_, &thret);
    posix__uninit_waitable_handle(&pthread_node->waiter_);

    /* �����߳��˳�����δ����Ĳ����ڴ�, ��ԭ����Ϊ��ʼ״̬ */
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
    
    /* �������� * 2 +2 ��ʲô�� */
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

    /* �������� * 2 +2 ��ʲô�� */
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

    /* ʹ�����ü�������֤���е��ù� INIT ��ʹ���߾����Ͽɷ���ʼ�� */
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

    /* ��ʹ�þ������ֵ��ϣ���̼߳��� */
    if (NULL == (pthread_node_selected = &pthread_node_set[hld % pthread_node_count])) {
        return NULL;
    }

    /* �����ϣ�����߳��ǻ�Ծ״̬�� �����ֱ��ʹ�� */
    if (pthread_node_selected->actived_) {
        return pthread_node_selected;
    }

    /* ָ���̲߳����ڻ�Ծ״̬, ��ͳ�ﵽ��һ���Ѽ����߳� */
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
    
    /* �̵߳��ȷ�ʽ�� ���ö���̶��߳�
     * IO ����ӵ���Լ���һ�����̺߳�һ��д�߳�
     * ���κ�һ����/д�̶߳����ܱ����IO����ӵ��
     *  */
    if (kTaskType_Read == ttype || kTaskType_Destroy == ttype) {
        pthread_node = select_worker(hld, pthread_manager.recv_, pthread_manager.recv_thcnt_);
    } else if (kTaskType_Write == ttype) {
        pthread_node = select_worker(hld, pthread_manager.send_, pthread_manager.send_thcnt_);
    }
    
    /* �������� ָ����������� NCB ���� */
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
        /* IO ��������̵߳������б� */
        posix__pthread_mutex_lock(&pthread_node->task_lock_);
        list_add_tail(&task->link_, &pthread_node->task_head_);
        posix__pthread_mutex_unlock(&pthread_node->task_lock_);

        /* ֪ͨ�ȴ��е��߳�, ����ִ�� */
        posix__sig_waitable_handle(&pthread_node->waiter_);
        return 0;
    }
    
    free(task);
    return -1;
}