#include "wpool.h"

#include "object.h"

#include "posix_wait.h"
#include "posix_atomic.h"
#include "posix_ifos.h"

#include "ncb.h"
#include "tcp.h"
#include "mxx.h"
#include "fifo.h"

struct wpool {
    posix__pthread_t thread;
    posix__pthread_mutex_t mutex;
    posix__waitable_handle_t signal;
    struct list_head tasks; /* struct wptask::link */
    int task_list_size;
    int actived;
};

struct wptask {
    objhld_t hld;
    struct wpool *thread;
    struct list_head link;
};

static objhld_t tcphld = -1;
static objhld_t udphld = -1;

static void __wp_add_task(struct wptask *task)
{
    struct wpool *wpptr;

    if (task) {
        wpptr = task->thread;
        INIT_LIST_HEAD(&task->link);
        posix__pthread_mutex_lock(&wpptr->mutex);
        list_add_tail(&task->link, &wpptr->tasks);
        ++wpptr->task_list_size;
        posix__pthread_mutex_unlock(&wpptr->mutex);
    }
}

static struct wptask *__wp_get_task(struct wpool *wpptr)
{
    struct wptask *task;

    posix__pthread_mutex_lock(&wpptr->mutex);
    if (NULL != (task = list_first_entry_or_null(&wpptr->tasks, struct wptask, link))) {
         --wpptr->task_list_size;
        list_del(&task->link);
        INIT_LIST_HEAD(&task->link);
    }
    posix__pthread_mutex_unlock(&wpptr->mutex);

    return task;
}

static int __wp_exec(struct wptask *task)
{
    int retval;
    ncb_t *ncb;
    int (*ncb_write)(struct _ncb *);

    assert(NULL != task);

    retval = -1;

    ncb = objrefr(task->hld);
    if (!ncb) {
        return -ENOENT;
    }

    ncb_write = posix__atomic_get(&ncb->ncb_write);
    if (ncb_write) {
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
        retval = ncb_write(ncb);

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
                __wp_add_task(task);
            }
        }
    }

    objdefr(ncb->hld);
    return retval;
}

static void *__wp_run(void *p)
{
    struct wptask *task;
    struct wpool *wpptr;
    int retval;

    wpptr = (struct wpool *)p;
    nis_call_ecr("[nshost.wpool.init] LWP:%u startup.", posix__gettid());

    while (wpptr->actived) {
        retval = posix__waitfor_waitable_handle(&wpptr->signal, 10);
        if ( retval < 0) {
            break;
        }

        /* reset wait object to block status immediately when the wait object timeout */
        if ( 0 == retval ) {
            posix__block_waitable_handle(&wpptr->signal);
        }

        /* complete all write task when once signal arrived,
            no matter which thread wake up this wait object */
        while ((NULL != (task = __wp_get_task(wpptr)) ) && wpptr->actived) {
            if (__wp_exec(task) <= 0) {
                free(task);
            }
        }
    }

    nis_call_ecr("[nshost.pool.wpool] LWP:%u terminated.", posix__gettid());
    pthread_exit((void *) 0);
    return NULL;
}

static int __wp_init(struct wpool *wpptr)
{
    INIT_LIST_HEAD(&wpptr->tasks);
    posix__init_notification_waitable_handle(&wpptr->signal);
    posix__pthread_mutex_init(&wpptr->mutex);
    wpptr->task_list_size = 0;
    wpptr->actived = 1;
    if (posix__pthread_create(&wpptr->thread, &__wp_run, (void *)wpptr) < 0 ) {
        nis_call_ecr("[nshost.pool.__wp_init] fatal error occurred syscall pthread_create(3), error:%d", errno);
        return -1;
    }

    return 0;
}

static void __wp_uninit(objhld_t hld, void *udata)
{
    struct wpool *wpptr;
    int *retval;
    struct wptask *task;

    wpptr = (struct wpool *)udata;
    assert(wpptr);

    /* This is an important judgment condition.
        when @__sync_bool_compare_and_swap failed in @wp_init, the mutex/condition_variable will notbe initialed,
        in this case, wait function block the calling thread and @wp_uninit progress cannot continue */
    if (wpptr->actived) {
        wpptr->actived = 0;
        posix__sig_waitable_handle(&wpptr->signal);
        posix__pthread_join(&wpptr->thread, (void **)&retval);

        /* clear the tasks which too late to deal with */
        posix__pthread_mutex_lock(&wpptr->mutex);
        while (NULL != (task = __wp_get_task(wpptr))) {
            free(task);
        }
        posix__pthread_mutex_unlock(&wpptr->mutex);

        INIT_LIST_HEAD(&wpptr->tasks);
        posix__uninit_waitable_handle(&wpptr->signal);
        posix__pthread_mutex_uninit(&wpptr->mutex);
    }

    if (!__sync_bool_compare_and_swap(&tcphld, hld, -1)) {
        __sync_bool_compare_and_swap(&udphld, hld, -1);
    }
}

void wp_uninit(int protocol)
{
    objhld_t *hldptr;

    hldptr = ((IPPROTO_TCP ==protocol ) ? &tcphld : ((IPPROTO_UDP == protocol || ETH_P_ARP == protocol) ? &udphld : NULL));
    if (hldptr) {
        if (*hldptr >= 0) {
            objclos(*hldptr);
        }
    }
}

int wp_init(int protocol)
{
    int retval;
    struct wpool *wpptr;
    objhld_t hld, *hldptr;

    hldptr = ((IPPROTO_TCP ==protocol ) ? &tcphld :
                ((IPPROTO_UDP == protocol || ETH_P_ARP == protocol) ? &udphld : NULL));
    if (!hldptr) {
        return -EPROTOTYPE;
    }

    /* judgment thread-unsafe first, handle the most case of interface rep-calls,
        this case NOT mean a error */
    if (*hldptr >= 0) {
        return EALREADY;
    }

    hld = objallo(sizeof(struct wpool), NULL, &__wp_uninit, NULL, 0);
    if (hld < 0) {
        return -1;
    }

    if (!__sync_bool_compare_and_swap(hldptr, -1, hld)) {
        objclos(hld);
        return EALREADY;
    }

    wpptr = objrefr(*hldptr);
    if (!wpptr) {
        return -ENOENT;
    }

    retval = __wp_init(wpptr);
    objdefr(*hldptr);
    return retval;
}

int wp_queued(void *ncbptr)
{
    struct wptask *task;
    struct wpool *wpptr;
    ncb_t *ncb;
    objhld_t hld;
    int retval;
    int protocol;

    ncb = (ncb_t *)ncbptr;
    if (!ncb) {
        return -EINVAL;
    }

    protocol = ncb->protocol;
    hld = ((IPPROTO_TCP == protocol ) ? tcphld :
                ((IPPROTO_UDP == protocol || ETH_P_ARP == protocol) ? udphld : -1));
    if (hld < 0) {
        return -ENOENT;
    }

    wpptr = objrefr(hld);
    if (!wpptr) {
        return -ENOENT;
    }

    do {
        if (NULL == (task = (struct wptask *)malloc(sizeof(struct wptask)))) {
            retval = -ENOMEM;
            break;
        }

        task->hld = ncb->hld;
        task->thread = wpptr;
        __wp_add_task(task);

        /* use local variable to save the thread object, because @task maybe already freed by handler now */
        posix__sig_waitable_handle(&wpptr->signal);
        retval = 0;
    } while (0);

    objdefr(hld);
    return retval;
}
