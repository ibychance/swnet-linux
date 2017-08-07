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

/*
 * д��(����)�������ӽӿڴ�������ѹ����еı׶�
 * 1. ������̵߳��� tcp_write, ��Ӧ�ò㲻����˳���ǰ���£� nshost�޷������Ⱥ�˳���յ����̵߳��Ⱥ���ִ��
 * 2. ÿ�����������Ҫ�����������߳��л������� �����ڷ��ͻ�������ԣ������£� ��ȫû�б�Ҫ
 * 
 * �ڽӿڲ���� if_blocked �жϵĲ���:
 * 1, tcp_write ��ֱ�ӵ��� send, udp_write ��ֱ�ӵ��� sendto, ��ֱ��ʹ�õ����̵߳��߳̿ռ�
 * 2, ���ϵͳ���÷��� EAGAIN, ����λ��ncb�ı�ǣ� ͬʱ��δ��ɵķ������ݻ��������
 *     2.1 ���� EAGAIN �� �� blocked ������ǰ��tcp_write/udp_write ��Ϊ���Ϊֱ�����
 *     2.2 EPOLL_OUT �¼���Ӧ�� �ӻ�������Ƚ��ȳ�ȡ�����������ݰ�������send/sendto ���ԣ� ֱ��ȫ�������������ϻ����ٴ� EAGAIN
 *     2.3 ���ȫ�������������ϣ� ������ blocked ��ǣ� ��������߳�ֱ�� send/sendto
 * 
 * ��Ҫ���еĲ�����:
 * 1. ����EAGAIN ���ٹ�ע EPOLL_OUT �¼��� �Ƿ���ȷ���¼���ץ��
 */

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

static int run_task(struct task_node_t *task) {
    objhld_t hld;
    int retval;
    ncb_t *ncb;
    
    assert(NULL != task);
    
    hld = task->hld;
    
    ncb = objrefr(hld);
    if (!ncb) {
        return -1;
    }
    
    /* ��ǰ״̬ΪIO���룬������Ӧ�κε� TxTest ���� */
    if (ncb_if_wblocked(ncb)){
        if (task->type == kTaskType_TxTest) {
            objdefr(hld);
            return EAGAIN;
        }
    }
    
    /*
     * �����ߵ��˴���ֻ�����������
     * 1. û�з���IO����
     * 2. ������IO���룬�����յ������� EPOLL �� TxOrder, ����������Ӧ
     */
    retval = ncb->ncb_write(ncb);
    
    /* �����޿���ȵ�ϵͳ����, �����ӽ����ر� */
    if(retval < 0){
        objclos(ncb->hld);
    }
    
    /* ���νڵ�˳��д���ں�  */
    else if (0 == retval) {
        
        /* �������IO�����ڼ䷢�������д�룬��ȡ��IO���룬ͬʱ�л�EPOLL��ע����������
         */
        if (task->type == kTaskType_TxOrder && ncb_if_wblocked(ncb) ) {
            ncb_cancel_wblock(ncb);
            iomod(ncb, kPollMask_Read);
        }
    }
    
    /* 
     * ���� EAGAIN�� �����Ѿ�������IO���룬��Ϊд��������������ͬһ���߳������Ľ��У���˲������̰߳�ȫ����
     * ����IO���룬 ͬʱ��EPOLL����Ϊ��עд�뻺����
     */
    else if (EAGAIN == retval ){
        ncb_mark_wblocked(ncb);
        iomod(ncb, kPollMask_Read | kPollMask_Write);
    }
    
    /* û���κ�������Ҫд�룬 �����ֿ� */
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

        /* ���̷��� 0. ��Ҫ���� task �ڴ� */
        while (NULL != (task = get_task(thread))) {
            run_task(task);
            free(task);
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

    /* ʹ�����ü�������֤���е��ù� INIT ��ʹ���߾����Ͽɷ���ʼ�� */
    if (0 != posix__atomic_dec(&refcnt)) {
        return;
    }
    
    write_pool.stop = posix__true;
    for (i = 0; i < write_pool.write_thread_count; i++){
        posix__pthread_join(&write_pool.write_threads[i].thread, &retval);
        
        /* ������������������� */
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
        return -ENOMEM;
    }
    task->hld = hld;
    task->type = type;
    
    thread = &write_pool.write_threads[hld % write_pool.write_thread_count];
    
    add_task(thread, task);
    posix__sig_waitable_handle(&thread->task_signal);
    return 0;
}