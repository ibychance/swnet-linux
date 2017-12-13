#include "fque.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

void fque_init(struct tx_fifo *fque) {
    if (fque) {
        INIT_LIST_HEAD(&fque->head);
        fque->size = 0;
        posix__pthread_mutex_init(&fque->lock);
    }
}

void fque_uninit(struct tx_fifo *fque) {
    struct tx_node *node;

    if (fque) {
        posix__pthread_mutex_lock(&fque->lock);
        while ((node = list_first_entry_or_null(&fque->head, struct tx_node, link)) != NULL) {
            list_del(&node->link);
            if (node->data) {
                free(node->data);
            }
            free(node);
        }
        posix__pthread_mutex_unlock(&fque->lock);
        posix__pthread_mutex_release(&fque->lock);
    }
}

int fque_priority_push(struct tx_fifo *fque, unsigned char *data, int cb, int offset, const struct sockaddr_in *target) {
    struct tx_node *node;
    int retval;

    if (!fque || !data || cb <= 0) {
        return -EINVAL;
    }

    node = (struct tx_node *) malloc(sizeof (struct tx_node));
    if (!node) {
        return -ENOMEM;
    }
    node->data = data;
    node->wcb = cb;
    node->offset = offset;
    if (target) {
        memcpy(&node->udp_target, target, sizeof (node->udp_target));
    }

    posix__pthread_mutex_lock(&fque->lock);
    list_add(&node->link, &fque->head);
    retval = ++fque->size;
    posix__pthread_mutex_unlock(&fque->lock);

    return retval;
}

/* 注意， 内部不对数据作深拷贝*/
int fque_push(struct tx_fifo *fque, unsigned char *data, int cb, const struct sockaddr_in *target) {
    struct tx_node *node;
    int retval;

    if (!fque || !data || cb <= 0) {
        return -1;
    }

    node = (struct tx_node *) malloc(sizeof (struct tx_node));
    if (!node) {
        return -1;
    }
    node->data = data;
    node->wcb = cb;
    node->offset = 0;
    if (target) {
        memcpy(&node->udp_target, target, sizeof (node->udp_target));
    }

    posix__pthread_mutex_lock(&fque->lock);
    list_add_tail(&node->link, &fque->head);
    retval = ++fque->size;
    posix__pthread_mutex_unlock(&fque->lock);

    return retval;
}

int fque_revert(struct tx_fifo *fque, struct tx_node *node) {
    int retval;

    if (!fque || !node) {
        return -EINVAL;
    }

    posix__pthread_mutex_lock(&fque->lock);
    list_add(&node->link, &fque->head);
    retval = ++fque->size;
    posix__pthread_mutex_unlock(&fque->lock);

    return retval;
}

struct tx_node *fque_get(struct tx_fifo *fque) {
    struct tx_node *node;

    if (!fque) {
        return NULL;
    }

    posix__pthread_mutex_lock(&fque->lock);
    if (NULL != (node = list_first_entry_or_null(&fque->head, struct tx_node, link))) {
        list_del(&node->link);
        INIT_LIST_HEAD(&node->link);
        --fque->size;
    }
    posix__pthread_mutex_unlock(&fque->lock);

    return node;
}

extern
int fque_size(struct tx_fifo *fque) {
    int retval;

    retval = -1;

    if (fque) {
        posix__pthread_mutex_lock(&fque->lock);
        retval = fque->size;
        posix__pthread_mutex_unlock(&fque->lock);
    }

    return retval;
}