#include "wpool.h"

#include "object.h"

#include "posix_wait.h"
#include "posix_atomic.h"
#include "posix_ifos.h"

#include "ncb.h"
#include "tcp.h"
#include "mxx.h"
#include "fifo.h"

struct wthread {
    posix__pthread_t thread;
    posix__pthread_mutex_t mutex;
    posix__waitable_handle_t signal;
    struct list_head tasks; /* struct task_node::link */
    int task_list_size;
    int stop;
};

struct task_node {
    objhld_t hld;
    struct wthread *thread;
    struct list_head link;
};

static objhld_t wphld_tcp = -1;
static objhld_t wphld_udp = -1;

static void __add_task(struct task_node *task)
{
    struct wthread *wpthptr;

    if (task) {
        wpthptr = task->thread;
        INIT_LIST_HEAD(&task->link);
        posix__pthread_mutex_lock(&wpthptr->mutex);
        list_add_tail(&task->link, &wpthptr->tasks);
        ++wpthptr->task_list_size;
        posix__pthread_mutex_unlock(&wpthptr->mutex);
    }
}

static struct task_node *__get_task(struct wthread *wpthptr)
{
    struct task_node *task;

    posix__pthread_mutex_lock(&wpthptr->mutex);
    if (NULL != (task = list_first_entry_or_null(&wpthptr->tasks, struct task_node, link))) {
         --wpthptr->task_list_size;
        list_del(&task->link);
        INIT_LIST_HEAD(&task->link);
    }
    posix__pthread_mutex_unlock(&wpthptr->mutex);

    return task;
}

static int __wp_exec(struct task_node *task)
{
    int retval;
    ncb_t *ncb;

    assert(NULL != task);

    retval = -1;

    ncb = objrefr(task->hld);
    if (!ncb) {
        return -ENOENT;
    }

    if (ncb->ncb_write) {
        /*
         * if the return value of @ncb_write equal to -1, that means system call maybe error, this link will be close
         *
         * if the return value of @ncb_write equal to -EAGAIN, set write IO blocked. this ncb object willbe switch to focus on EPOLLOUT | EPOLLIN
         * bacause the write operation object always takes place in the same thread context, there is no thread security problem.
         * for the data which has been reverted, write tasks will be obtained through event triggering of EPOLLOUT
         *
         * if the return value of @ncb_write equal to zero, it means the queue of pending data node is empty, not any send operations are need.
         * here can be consumed the task where allocated by kTaskType_TxOrder sucessful completed
         *
         * if the return value of @ncb_write greater than zero, it means the data segment have been written to system kernel
         * @retval is the total bytes that have been written
         */
        retval = ncb->ncb_write(ncb);

        /* fatal error cause by syscall, close this link */
        if(-1 == retval) {
            objclos(ncb->hld);
        } else if (-EAGAIN == retval ) {
            ; /* when EAGAIN occurred, wait for next EPOLLOUT event, just ok */
        } else if (0 == retval) {
            ;/* nop, no item in fifo now */
        } else {
            /* on success, we need to append task to the tail of @fifo again, until all pending data have been sent
                in this case, @__wp_run should not free the memory of this task  */
            if (fifo_pop(ncb, NULL) > 0) {
                __add_task(task);
            }
        }
    }

    objdefr(ncb->hld);
    return retval;
}

static void *__wp_run(void *p)
{
    struct task_node *task;
    struct wthread *wpthptr;
    int retval;

    wpthptr = (struct wthread *)p;
    nis_call_ecr("[nshost.wpool.init] LWP:%u startup.", posix__gettid());

    while (!wpthptr->stop) {
        retval = posix__waitfor_waitable_handle(&wpthptr->signal, 10);
        if ( retval < 0) {
            break;
        }

        /* reset wait object to block status immediately when the wait object timeout */
        if ( 0 == retval ) {
            posix__block_waitable_handle(&wpthptr->signal);
        }

        /* complete all write task when once signal arrived,
            no matter which thread wake up this wait object */
        while ((NULL != (task = __get_task(wpthptr)) ) && !wpthptr->stop) {
            if (__wp_exec(task) <= 0) {
                free(task);
            }
        }
    }

    nis_call_ecr("[nshost.pool.wpool] LWP:%u terminated.", posix__gettid());
    pthread_exit((void *) 0);
    return NULL;
}

static int __wp_init(struct wthread *wpthptr)
{
    INIT_LIST_HEAD(&wpthptr->tasks);
    posix__init_notification_waitable_handle(&wpthptr->signal);
    posix__pthread_mutex_init(&wpthptr->mutex);
    wpthptr->task_list_size = 0;
    if (posix__pthread_create(&wpthptr->thread, &__wp_run, (void *)wpthptr) < 0 ) {
        nis_call_ecr("[nshost.pool.__wp_init] fatal error occurred syscall pthread_create(3), error:%d", errno);
        return -1;
    }

    return 0;
}

static void __wp_uninit(objhld_t hld, void *udata)
{
    struct wthread *wpthptr;
    int *retval;
    struct task_node *task;

    wpthptr = (struct wthread *)udata;
    assert(wpthptr);

    wpthptr->stop = 1;
    posix__sig_waitable_handle(&wpthptr->signal);
    posix__pthread_join(&wpthptr->thread, (void **)&retval);

    /* clear the tasks which too late to deal with */
    posix__pthread_mutex_lock(&wpthptr->mutex);
    while (NULL != (task = __get_task(wpthptr))) {
        free(task);
    }
    posix__pthread_mutex_unlock(&wpthptr->mutex);

    INIT_LIST_HEAD(&wpthptr->tasks);
    posix__uninit_waitable_handle(&wpthptr->signal);
    posix__pthread_mutex_uninit(&wpthptr->mutex);
}

void wp_uninit(int protocol)
{
    if (kProtocolType_TCP ==protocol ) {
        objclos(wphld_tcp);
        wphld_tcp = -1;
    }

    if (kProtocolType_UDP == protocol ) {
        objclos(wphld_udp);
        wphld_udp = -1;
    }
}

int wp_init(int protocol)
{
    int retval;
    struct wthread *wpthptr;
    objhld_t *hldptr;

    hldptr = NULL;

    if (kProtocolType_TCP ==protocol ) {
        hldptr = &wphld_tcp;
    }

    if (kProtocolType_UDP == protocol ) {
        hldptr = &wphld_udp;
    }

    if (!hldptr) {
        return -1;
    }

    if (*hldptr >= 0) {
        return 0;
    }

    *hldptr = objallo(sizeof(struct wthread), NULL, &__wp_uninit, NULL, 0);
    if (*hldptr < 0) {
        return -1;
    }

    wpthptr = objrefr(*hldptr);
    assert(wpthptr);

    retval = __wp_init(wpthptr);
    objdefr(*hldptr);
    return retval;
}

int wp_queued(void *ncbptr)
{
    struct task_node *task;
    struct wthread *wpthptr;
    ncb_t *ncb;
    objhld_t wphld;
    int retval;

    ncb = (ncb_t *)ncbptr;
    if (!ncb) {
        return -EINVAL;
    }

    wphld = -1;
    if (ncb->proto_type == kProtocolType_TCP) {
        wphld = wphld_tcp;
    }

    if (ncb->proto_type == kProtocolType_UDP) {
        wphld = wphld_udp;
    }

    wpthptr = objrefr(wphld);
    if (!wpthptr) {
        return -1;
    }

    do {
        if (NULL == (task = (struct task_node *)malloc(sizeof(struct task_node)))) {
            retval = -ENOMEM;
            break;
        }

        task->hld = ncb->hld;
        task->thread = wpthptr;
        __add_task(task);

        /* use local variable to save the thread object, because @task maybe already freed by handler now */
        posix__sig_waitable_handle(&wpthptr->signal);
        retval = 0;
    } while (0);

    objdefr(wphld);
    return retval;
}
