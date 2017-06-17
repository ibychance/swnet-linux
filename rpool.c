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
    
    /* ����ر��¼� */
    if (task->type == kTaskType_Destroy) {
        objclos(hld);
        return 0;
    }
    
    retval = -1;
    
    /* �����Ѿ������ڣ� �����޷����� */
    ncb = objrefr(hld);
    if (!ncb) {
        return -1;
    }

    /* ����������������У� ���� Rx ������������У�� 
     * У��������Ϊ��Ҫ��Ϊ���������Զ��������̵߳ı���������ִ��������ֹ*/
    if (ncb->proto_type == kProtocolType_TCP) {
        posix__pthread_mutex_lock(&ncb->rx_prot_lock);
        do {
            if (!ncb->rx_running) {
                ncb->rx_running = posix__true;
                break;
            }

            /* ����ִ���У� �յ��������� ��ʱ�ж� order �����Ƿ�Ϊ��
             * Ϊ���������������
             * ��Ϊ��������ִ��, ͬʱ�ݼ� order ���� 
             *
             * ����߼���Ҫ���ڱ�֤���������龰:
             * tcp_rx �����ж������ݣ����ں˻��������գ�����һ�� RxTest ����� �Ѿ�����EAGAIN, ���ǻ�û�н��"������"״̬
             * ��ʱ epoll ���������Ҹ�������̵߳õ����ȵ��ȣ� �ܿ���ߵ��������
             * �������ƾtcp_rx ��״̬���жϣ� �����Ա�֤����ʧ����
             * ���еĲ�������Ա�֤ÿ��epoll�����������������̺�����һ�� RxTest */
            if ((task->type == kTaskType_RxTest) && (ncb->rx_order_count > 0)) {
                //printf("read decrement.\n");
                --ncb->rx_order_count;
                break;
            }

            /* û���κ� order ������ RxTest ����˵�������������� Rx ���̵��Լ�
             * Rx ����Ҫ����� EAGAIN Ϊֹ, ֻҪû�з��� EAGAIN, Rx ���̶��ᴥ�������Լ� */
            if ((task->type == kTaskType_RxTest) && (ncb->rx_order_count == 0)) {
                break;
            }
            
            /* ����ִ���У���Ҫ������ epoll ���������ۼӼ�������
             * ���ں��������н���ת��Ϊ RxTest ����, ���������񽫱���ֹ */
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
    
    /* ����˳�����ں˻������������ݣ��������񣬷����Լ�
     * Ϊ�˷�ֹ�κ�һ�����Ӷ���, ������κ����Ӷ�����recv��Ĵ���, ���ǲ���׷������ķ��� */
    if (0 == retval){
        //printf("[%u] read ok %llu\n", posix__gettid(), posix__clock_gettime());
        task->type = kTaskType_RxTest;
        add_task(task);
    }
    
    /* ����ϵͳ���ô���ȣ� �޷��ָ����ڲ����� ���ӽ����ر� */
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

        /* �������IO��ֹ(����ֵ����0)�� ��������
             * ������������� */
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

    /* �������� * 2 +2 ��ʲô�� */
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

    /* ʹ�����ü�������֤���е��ù� INIT ��ʹ���߾����Ͽɷ���ʼ�� */
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

    /* ������������������� */
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
    
    /* �������� ָ����������� NCB ���� */
    if (NULL == (task = (struct task_node_t *) malloc(sizeof (struct task_node_t)))) {
        return -1;
    }
    task->type = type;
    task->hld = hld;

    /* ֪ͨ�ȴ��е��߳�, ����ִ�� */
//    posix__pthread_mutex_lock(&read_pool.task_lock);
//    list_add_tail(&task->link, &read_pool.task_list);
//    ++read_pool.task_list_size;
//    posix__pthread_mutex_unlock(&read_pool.task_lock);
    
    add_task(task);
    posix__sig_waitable_handle(&read_pool.task_signal);
    return 0;
}