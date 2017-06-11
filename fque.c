#include "fque.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

void fque_init(packet_fifo_t *fque) {
    if (fque) {
        INIT_LIST_HEAD(&fque->head_);
        fque->size_ = 0;
        posix__pthread_mutex_init(&fque->lock_);
    }
}

void fque_uninit(packet_fifo_t *fque) {
    packet_node_t *node;

    if (fque) {
        posix__pthread_mutex_lock(&fque->lock_);
        while ((node = list_first_entry_or_null(&fque->head_, packet_node_t, link_)) != NULL) {
            free(node->packet_);
            list_del(&node->link_);
            free(node);
        }
        posix__pthread_mutex_unlock(&fque->lock_);
        posix__pthread_mutex_release(&fque->lock_);
    }
}

/* 注意， 内部不对数据作深拷贝*/
int fque_push(packet_fifo_t *fque, unsigned char *data, int cb, const struct sockaddr_in *target) {
    packet_node_t *node;
    int retval;

    if (!fque || !data || cb <= 0) {
        return -1;
    }

    node = (packet_node_t *) malloc(sizeof (packet_node_t));
    if (!node) {
        return -1;
    }
    node->packet_ = data;
    node->wcb_ = cb;
    node->offset_ = 0;
    if (target) {
        memcpy(&node->target_, target, sizeof (node->target_));
    }

    posix__pthread_mutex_lock(&fque->lock_);
    list_add_tail(&node->link_, &fque->head_);
    retval = ++fque->size_;
    posix__pthread_mutex_unlock(&fque->lock_);

    return retval;
}

int fque_revert(packet_fifo_t *fque, packet_node_t *node) {
    int retval;

    if (!fque || !node) {
        return -EINVAL;
    }

    posix__pthread_mutex_lock(&fque->lock_);
    list_add(&node->link_, &fque->head_);
    retval = ++fque->size_;
    posix__pthread_mutex_unlock(&fque->lock_);

    return retval;
}

packet_node_t *fque_get(packet_fifo_t *fque) {
    packet_node_t *node;

    if (!fque) {
        return NULL;
    }

    posix__pthread_mutex_lock(&fque->lock_);
    if (NULL != (node = list_first_entry_or_null(&fque->head_, packet_node_t, link_))) {
        list_del(&node->link_);
        INIT_LIST_HEAD(&node->link_);
        --fque->size_;
    }
    posix__pthread_mutex_unlock(&fque->lock_);

    return node;
}

extern
int fque_size(packet_fifo_t *fque) {
    int retval;

    retval = -1;

    if (fque) {
        posix__pthread_mutex_lock(&fque->lock_);
        retval = fque->size_;
        posix__pthread_mutex_unlock(&fque->lock_);
    }

    return retval;
}