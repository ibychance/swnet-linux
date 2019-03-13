#include "fifo.h"

#include "mxx.h"
#include "io.h"

#define MAXIMUM_FIFO_SIZE       (100)

void fifo_init(ncb_t *ncb) {
    struct tx_fifo *fifo;

    if (ncb) {
        fifo = &ncb->fifo;
        fifo->blocking = 0;
        fifo->size = 0;
        posix__pthread_mutex_init(&fifo->lock);
        INIT_LIST_HEAD(&fifo->head);
    }
}

void fifo_uninit(ncb_t *ncb) {
    struct tx_node *node;
    struct tx_fifo *fifo;

    if (ncb) {
        fifo = &ncb->fifo;
        posix__pthread_mutex_lock(&fifo->lock);
        while ((node = list_first_entry_or_null(&fifo->head, struct tx_node, link)) != NULL) {
            list_del(&node->link);
            if (node->data) {
                free(node->data);
            }
            free(node);
        }
        posix__pthread_mutex_unlock(&fifo->lock);
        posix__pthread_mutex_release(&fifo->lock);
    }
}

int fifo_queue(ncb_t *ncb, struct tx_node *node) {
    int n;
    struct tx_fifo *fifo;

    if (!ncb || !node) {
        return -1;
    }

    fifo = &ncb->fifo;
    n = -1;

    posix__pthread_mutex_lock(&fifo->lock);
    do {
        if (fifo->size >= MAXIMUM_FIFO_SIZE) {
            n = -EBUSY;
            break;
        }
        list_add_tail(&node->link, &fifo->head);
        n = ++fifo->size;

        /* queue item into fifo.
         * it is ensure the EAGAIN event was happened.
         * the calling thread should change the epoll to EPOLLOUT mode */
        if (0 == fifo->blocking) {
            fifo->blocking = 1;
            iomod(ncb, EPOLLIN | EPOLLOUT);
            nis_call_ecr("[nshost.fifo.fifo_queue] set IO blocking,link:%lld", ncb->hld);
        }
    } while(0);

    posix__pthread_mutex_unlock(&fifo->lock);
    return n;
}

int fifo_top(ncb_t *ncb, struct tx_node **node) {
    struct tx_node *front;
    struct tx_fifo *fifo;

    if (!ncb || !node) {
        return -1;
    }

    fifo = &ncb->fifo;
    front = NULL;

    posix__pthread_mutex_lock(&fifo->lock);
    if (NULL != (front = list_first_entry_or_null(&fifo->head, struct tx_node, link))) {
        *node = front;
    }
    posix__pthread_mutex_unlock(&fifo->lock);

    return ((NULL == front) ? -1 : 0);
}

int fifo_pop(ncb_t *ncb, struct tx_node **node) {
    struct tx_node *front;
    struct tx_fifo *fifo;
    int remain;

    if (!ncb) {
        return -1;
    }

    fifo = &ncb->fifo;
    front = NULL;

    posix__pthread_mutex_lock(&fifo->lock);
    if (NULL != (front = list_first_entry_or_null(&fifo->head, struct tx_node, link))) {
        assert(fifo->size > 0);
        list_del(&front->link);
        INIT_LIST_HEAD(&front->link);
        remain = --fifo->size;

        /* the calling thread should change the epoll to EPOLLIN mode */
        if ((0 == remain) && (1 == fifo->blocking)) {
            fifo->blocking = 0;
            iomod(ncb, EPOLLIN);
            nis_call_ecr("[nshost.fifo.fifo_pop] cancel IO blocking,link:%lld", ncb->hld);
        }
    }
    posix__pthread_mutex_unlock(&fifo->lock);

    if (front) {
        if (node) {
            *node = front;
        }

        if (front->data) {
            free(front->data);
        }
        free(front);
        return 1;
    }

    return 0;
}

int fifo_is_blocking(ncb_t *ncb) {
    struct tx_fifo *fifo;
    int blocking;

    if (!ncb) {
        return -1;
    }

    fifo = &ncb->fifo;

    posix__pthread_mutex_lock(&fifo->lock);
    blocking = fifo->blocking;
    posix__pthread_mutex_unlock(&fifo->lock);

    return blocking;
}