#include "fque.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

void fque_init(struct packet_fifo_t *fque) {
    if (fque) {
        INIT_LIST_HEAD(&fque->head);
        fque->size = 0;
        posix__pthread_mutex_init(&fque->lock);
    }
}

void fque_uninit(struct packet_fifo_t *fque) {
    struct packet_node_t *node;

    if (fque) {
        posix__pthread_mutex_lock(&fque->lock);
        while ((node = list_first_entry_or_null(&fque->head, struct packet_node_t, link)) != NULL) {
            free(node->data);
            list_del(&node->link);
            free(node);
        }
        posix__pthread_mutex_unlock(&fque->lock);
        posix__pthread_mutex_release(&fque->lock);
    }
}

int fque_priority_push(struct packet_fifo_t *fque, unsigned char *data, int cb, int offset, const struct sockaddr_in *target) {
    struct packet_node_t *node;
    int retval;

    if (!fque || !data || cb <= 0) {
        return -1;
    }

    node = (struct packet_node_t *) malloc(sizeof (struct packet_node_t));
    if (!node) {
        return -1;
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
int fque_push(struct packet_fifo_t *fque, unsigned char *data, int cb, const struct sockaddr_in *target) {
    struct packet_node_t *node;
    int retval;

    if (!fque || !data || cb <= 0) {
        return -1;
    }

    node = (struct packet_node_t *) malloc(sizeof (struct packet_node_t));
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

int fque_revert(struct packet_fifo_t *fque, struct packet_node_t *node) {
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

struct packet_node_t *fque_get(struct packet_fifo_t *fque) {
    struct packet_node_t *node;

    if (!fque) {
        return NULL;
    }

    posix__pthread_mutex_lock(&fque->lock);
    if (NULL != (node = list_first_entry_or_null(&fque->head, struct packet_node_t, link))) {
        list_del(&node->link);
        INIT_LIST_HEAD(&node->link);
        --fque->size;
    }
    posix__pthread_mutex_unlock(&fque->lock);

    return node;
}

extern
int fque_size(struct packet_fifo_t *fque) {
    int retval;

    retval = -1;

    if (fque) {
        posix__pthread_mutex_lock(&fque->lock);
        retval = fque->size;
        posix__pthread_mutex_unlock(&fque->lock);
    }

    return retval;
}