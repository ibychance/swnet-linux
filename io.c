#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/signal.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

#include <linux/unistd.h>
#include <linux/types.h>

#include "compiler.h"
#include "posix_thread.h"
#include "posix_atomic.h"
#include "posix_wait.h"
#include "posix_ifos.h"

#include "io.h"
#include "ncb.h"
#include "object.h"
#include "worker.h"
#include "clist.h"

/* 1024 is just a hint for the kernel */
#define EPOLL_SIZE    (1024)

struct epoll_object_t{
    int epfd;
    posix__boolean_t actived;
    posix__pthread_t thread; /* EPOLL �߳� */
    int load; /* ���̵߳ĵ�ǰ����ѹ����� */
} ;

struct epoll_object_manager_t {
    struct epoll_object_t *epos;
    int nprocs;
    int min_load;
    int min_index;
    posix__pthread_mutex_t lock_selection; /* ��ס���/��С����ɸѡ�����±���� */ 
};

static struct epoll_object_manager_t epmgr;
static int refcnt = 0;

static void io_run(struct epoll_event *evts, int sigcnt){
    int i;
    ncb_t *ncb;
    objhld_t hld;
    
    for (i = 0; i < sigcnt; i++) {
        hld = evts[i].data.fd;
        ncb = (ncb_t *)objrefr(hld);
        if (!ncb) {
            continue;
        }

        /*
         * �Ͽ��¼�
         */
        if (evts[i].events & EPOLLRDHUP) {
            objclos(ncb->hld);
        }

        /*
         * ��������:
         * 1. �����ݵ���
         * 2. �� syn ���󵽴�
         * 3. ��ȡ���������������ת��Ϊ�ǿ�
         * TCP ��������  cat /proc/sys/net/ipv4/tcp_rmem 
         * 
         * ��ע:
         * 1.9ϵ�а汾�������ɫ����epoll�߳�ֱ�Ӵ���������������
         * 2.��������epoll���ѵ��ϲ����Э���ܹ��������²���:
         *      2.1 ��Ӧepoll�¼������ں˻�����readֱ��EAGAIN
         *      2.2 Ӧ�ò���
         *      2.3 �ȴ��ϲ㴦�����
         *   �����У�2.1��2.2�� �����Ƿ�ת���̣߳���Ҫ�ļ���ʱ�䶼����ͬ��8ϵ�еĴ���ʽ��������
         *   ת���߳̾��ܿ��Ա����ϴٽ�epoll�����һ���������¼������յ��¼���Ĵ�������CPUʱ�䲢�������
         *   Ψһ��Ҫ��ע���ϲ�Ӧ���� on_recvdata �еĲ��������� ����ϲ���ú�ʱ������ ���������ǰ�߳��������������ļ����հ�
         * 3.���ǵ������Ŀ�����Ӧ�����������ϲ���ܷ�������������ҲӦ�����ϲ�Ͷ���̳߳أ����������²�����̳߳�
         */
        if (evts[i].events & EPOLLIN) {
            if (ncb->ncb_read) {
                if (ncb->ncb_read(ncb) < 0) {
                    objclos(ncb->hld);
                }
            }else{
                ncb_report_debug_information(ncb, "nullptr read address for EPOLLIN");
            }
        }

        /*
         * ETģʽ�£�EPOLLOUT���������У�
            1.��������-->������������
            2.ͬʱ����EPOLLOUT��EPOLLIN�¼� ʱ������IN �¼�����������˳��һ��OUT�¼���
            3.һ���ͻ���connect������accept�ɹ���ᴥ��һ��OUT�¼���

         * ע������:
         * 1. (EPOLLIN | EPOLLOUT) һ������ע�� ��ÿ��д�뻺�������� EPOLLIN ����Я������һ��, ������ܣ� �Ҳ����ײ��� oneshot
         * 2. ƽ�������ע EPOLLOUT
         * 3. һ��д��������� EAGAIN, ����һ��д���������ֻ���� EPOLLOUT ����(��ע״̬�л�)
         * TCP д������ cat /proc/sys/net/ipv4/tcp_wmem 
         * 
         * ����֤������������EAGAIN���ٹ�עEPOLLOUT�������ڻ������ɹ��ճ���õ�֪ͨ
         */
        if (evts[i].events & EPOLLOUT) {
            post_write_task(ncb->hld, kTaskType_TxOrder);
        }
        
        objdefr(hld);
    }
}

static void *epoll_proc(void *argv) {
    struct epoll_event evts[EPOLL_SIZE];
    int sigcnt;
    int errcode;
    struct epoll_object_t *epo;

    epo = (struct epoll_object_t *)argv;
    while (epo->actived) {
        sigcnt = epoll_wait(epo->epfd, evts, EPOLL_SIZE, -1);
        if (sigcnt < 0) {
            errcode = errno;

            /* EINTR��ʾ�����߼���ϵͳ���ô�ϣ�����һ��recv�޷���ɵĻ��������� */
            if (EINTR == errcode || EAGAIN == errcode) {
                continue;
            }
            printf("[EPOLL] error on epoll_wait, errno=%d.\n", errcode);
            break;
        }
        io_run(evts, sigcnt);
    }

    printf("[EPOLL] services trunk loop terminated.\n");
    return NULL;
}

int ioinit() {
    int i;

    if (posix__atomic_inc(&refcnt) > 1) {
        return 0;
    }

    /* ��һ���Ѿ��رյ�����ִ�� write, ���� EPIPE ��ͬʱ�� raise һ��SIGPIPE �źţ���Ҫ���Դ��� */
    signal(SIGPIPE, SIG_IGN);
    
    epmgr.nprocs = get_nprocs();
    if ( NULL == (epmgr.epos = (struct epoll_object_t *)malloc(sizeof(struct epoll_object_t) * epmgr.nprocs))) {
        posix__atomic_dec(&refcnt);
        return -1;
    }
    posix__pthread_mutex_init(&epmgr.lock_selection);
    epmgr.min_load = 0;
    epmgr.min_index = 0;
    
    for (i = 0; i < epmgr.nprocs; i++) {
        epmgr.epos[i].load = 0;
        epmgr.epos[i].epfd = epoll_create(EPOLL_SIZE);
        if (epmgr.epos[i].epfd < 0) {
            printf("[EPOLL] failed to allocate file descriptor.\n");
            epmgr.epos[i].actived = posix__false;
            continue;
        }
        
        /* active �ֶμ���Ϊ������Ч�Ե��жϷ���Ҳ��Ϊ���еĿ��Ʒ��� */
        epmgr.epos[i].actived = posix__true;
         if (posix__pthread_create(&epmgr.epos[i].thread, &epoll_proc, &epmgr.epos[i]) < 0) {
            epmgr.epos[i].actived = posix__false;
            close(epmgr.epos[i].epfd);
            epmgr.epos[i].epfd = -1;
        }
    }

    return 0;
}

void iouninit() {
    int i;
    
    if (0 == refcnt) {
        return;
    }

    /* ��Ҫ���г�ʼ�������߶��Ͽɷ���ʼ���� ����ʼ�����ܵ���ִ�� */
    if (posix__atomic_dec(&refcnt) > 0) {
        return;
    }
    
    if (!epmgr.epos) {
        return;
    }
    
    for (i = 0; i < epmgr.nprocs; i++){
        if (epmgr.epos[i].epfd > 0){
            close(epmgr.epos[i].epfd);
            epmgr.epos[i].epfd = -1;
        }
        
        if (epmgr.epos[i].actived){
           posix__atomic_xchange(&epmgr.epos[i].actived, posix__false);
           posix__pthread_join(&epmgr.epos[i].thread, NULL);
        }
    }
    
    free(epmgr.epos);
    epmgr.epos = NULL;
    
    posix__pthread_mutex_uninit(&epmgr.lock_selection);
}

/* ѡ��ǰѹ����С��EPO��Ϊ���ض���, ���ظö����EPFD�� �����κδ��󷵻�-1 */
static int io_select_object(){
    int epfd;
    int i;
    
    epfd = -1;
    posix__pthread_mutex_lock(&epmgr.lock_selection);
    epmgr.epos[epmgr.min_index].load++;
    epmgr.min_load++;
    epfd = epmgr.epos[epmgr.min_index].epfd;
    for (i = 0; i < epmgr.nprocs; i++){
        if ((i != epmgr.min_index) && (epmgr.epos[i].load < epmgr.min_load )){
            epmgr.min_index = i;
            epmgr.min_load = epmgr.epos[i].load;
            break;
        }
    }
    posix__pthread_mutex_unlock(&epmgr.lock_selection);
    return epfd;
}

int ioatth(void *ncbptr, enum io_poll_mask_t mask) {
    struct epoll_event e_evt;
    ncb_t *ncb;
    
    ncb = (ncb_t *)ncbptr;
    if (!ncb) {
        return -EINVAL;
    }
    
    e_evt.data.fd = ncb->hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP); 
    if (mask & kPollMask_Oneshot) {
        e_evt.events |= EPOLLONESHOT;
    }
    if (mask & kPollMask_Read) {
        e_evt.events |= EPOLLIN;
    }
    if (mask & kPollMask_Write){
        e_evt.events |= EPOLLOUT;
    }
    
    ncb->epfd = io_select_object();
    if (ncb->epfd < 0 ){
        return -1;
    }
    
    return epoll_ctl(ncb->epfd, EPOLL_CTL_ADD, ncb->sockfd, &e_evt);
}

int iomod(void *ncbptr, enum io_poll_mask_t mask ) {
    struct epoll_event e_evt;
    ncb_t *ncb;
    
    ncb = (ncb_t *)ncbptr;
    if (!ncb) {
        return -EINVAL;
    }

    e_evt.data.fd = ncb->hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP | EPOLLIN); 
    if (mask & kPollMask_Oneshot) {
        e_evt.events |= EPOLLONESHOT;
    }
    if (mask & kPollMask_Read) {
        e_evt.events |= EPOLLIN;
    }
    if (mask & kPollMask_Write){
        e_evt.events |= EPOLLOUT;
    }
    return epoll_ctl(ncb->epfd, EPOLL_CTL_MOD, ncb->sockfd, &e_evt);
}

void iodeth(void *ncbptr) {
    struct epoll_event evt;
    ncb_t *ncb;
    
    ncb = (ncb_t *)ncbptr;
    if (ncb) {
        epoll_ctl(ncb->epfd, EPOLL_CTL_DEL, ncb->sockfd, &evt);
    }
}

void ioclose(void *ncbptr) {
    ncb_t *ncb = (ncb_t *)ncbptr;
    if (!ncb){
        return;
    }
    
    if (ncb->sockfd > 0){
         if (ncb->epfd> 0){
            iodeth(ncb);
            ncb->epfd = -1;
        }
         
        shutdown(ncb->sockfd, 2);
        close(ncb->sockfd);
        ncb->sockfd = -1;
    }
}

int setasio(int fd) {
    int opt;

    if (fd < 0) {
        return -1;
    }

    opt = fcntl(fd, F_GETFL);
    if (opt < 0) {
        printf("[EPOLL] failed get file status flag,errno=%d.\n ", errno);
        return -1;
    }

    if (fcntl(fd, F_SETFL, opt | O_NONBLOCK) < 0) {
        printf("[EPOLL] failed set file status flag with non_block,errno=%d.\n", errno);
        return -1;
    }
    return 0;
}

int setsyio(int fd){
    int opt;

    if (fd < 0) {
        return -1;
    }

    opt = fcntl(fd, F_GETFL);
    if (opt < 0) {
        printf("[EPOLL] failed get file status flag,errno=%d.\n ", errno);
        return -1;
    }

    opt &= ~O_NONBLOCK;
    if (fcntl(fd, F_SETFL, opt) < 0) {
        printf("[EPOLL] failed set file status flag with syio,errno=%d.\n", errno);
        return -1;
    }
    return 0;
}