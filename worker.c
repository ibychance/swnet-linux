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
    
    /* �����Ѿ������ڣ� �����޷����� */
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
        
        /* ����û��ָ����д���̣� �����޷����� */
        if (task->type == kTaskType_RxOrder || task->type == kTaskType_RxAttempt) {
            
            handler = ncb->ncb_read;

            /* ����������������У� ���� Rx ������������У�� 
             * У��������Ϊ��Ҫ��Ϊ���������Զ��������̵߳ı����������ÿ� handler*/
            if (ncb->proto_type == kProtocolType_TCP) {
                posix__pthread_mutex_lock(&ncb->rx_prot_lock);
                do {
                    if (!ncb->rx_running) {
                        ncb->rx_running = posix__true;
                        break;
                    }

                    /* ����ִ���У���Ҫ������ epoll ���������ۼӼ�������
                     * ���ں��������н���ת��Ϊ attempt ���� */
                    if (task->type == kTaskType_RxOrder) {
                        ncb->rx_order_count++;
                        handler = NULL;
                        break;
                    }

                    /* ����ִ���У� �յ��������� ��ʱ�ж� order �����Ƿ�Ϊ��
                     * Ϊ���������������
                     * ��Ϊ��������ִ��, ͬʱ�ݼ� order ���� 
                     *
                     * ����߼���Ҫ���ڱ�֤���������龰:
                     * tcp_rx �����ж������ݣ����ں˻��������գ�����һ�� attempt ����� �Ѿ�����EAGAIN, ���ǻ�û�н��"������"״̬
                     * ��ʱ epoll ���������Ҹ�������̵߳õ����ȵ��ȣ� �ܿ���ߵ��������
                     * �������ƾtcp_rx ��״̬���жϣ� �����Ա�֤����ʧ����
                     * ���еĲ�������Ա�֤ÿ��epoll�����������������̺�����һ�� attempt */
                    if ((task->type == kTaskType_RxAttempt) && (ncb->rx_order_count > 0)) {
                        --ncb->rx_order_count;
                        break;
                    }

                    /* û���κ� order ������ attempt ����˵�������������� Rx ���̵��Լ�
                     * Rx ����Ҫ����� EAGAIN Ϊֹ, ֻҪû�з��� EAGAIN, Rx ���̶��ᴥ�������Լ� */
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
         * ������ȷ���ô������̷���ֵ����
         * 1. Rx:
         * 2. Tx:
         *  2.1 retval < 0 ��ʾ�����ڲ�����ϵͳ���ô�����޷��������޷�����Ĵ��� 
         * 2.2 retval == 0 ���óɹ��� ��/д�������ռȶ��ֽ������
         * 2.3 retval > 0(EAGAIN) (ͨ����д�������µ�)�޷�������ɣ�������Ҫ����       
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

        /* �������IO��ֹ(����ֵ����0)�� ��������
             * ������������� */
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

    /* �������� * 2 +2 ��ʲô�� */
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

    /* ʹ�����ü�������֤���е��ù� INIT ��ʹ���߾����Ͽɷ���ʼ�� */
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
    
    /* �������� ָ����������� NCB ���� */
    if (NULL == (task = (struct task_node_t *) malloc(sizeof (struct task_node_t)))) {
        return -1;
    }
    task->type = ttype;
    task->hld = hld;
    add_task(task);
    
    /* ֪ͨ�ȴ��е��߳�, ����ִ�� */
    posix__sig_waitable_handle(&task_thread_pool.task_signal);
    return 0;
}