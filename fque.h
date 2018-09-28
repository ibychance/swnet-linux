#ifndef FQUE_H_20170118
#define FQUE_H_20170118

/* forward queued */

#include <netinet/in.h>
#include <stdlib.h>
#include <memory.h>

#include "clist.h"
#include "posix_thread.h"

struct tx_node {
    unsigned char *data; /* 请求写入的数据缓冲区原始指针 */
    int wcb; /* 总共的写入大小 */
    int offset; /* 当前的完成写入偏移 */
    struct sockaddr_in udp_target; /* UDP 可用， 指定发送目标 */
    struct list_head link; /* 勾链 */
};

#define PACKET_NODE_FREE(node)  \
            do { if (!node) break;if ( node->data ) free(node->data);free(node); } while (0)

struct tx_fifo {
    struct list_head head;
    int size;
    posix__pthread_mutex_t lock;
} ;

extern
void fque_init(struct tx_fifo *fque);
extern
void fque_uninit(struct tx_fifo *fque);

extern
int fque_priority_push(struct tx_fifo *fque, unsigned char *data, int cb, int offset, const struct sockaddr_in *target);

/*
 * no deepcopy for @data, only heap memory can be used.
 */
extern
int fque_push(struct tx_fifo *fque, unsigned char *data, int cb, const struct sockaddr_in *target);

/* 
 * (未完成操作的节点)归还给 FIFO, 并且放置于队列头
 * 这种处理方式， 往往来源于头对象未能在移除周期完成操作
 *  */
extern
int fque_revert(struct tx_fifo *fque, struct tx_node *node);

/* 
 * 取出队列头对象， 如果队列为空， 返回NULL
 * 操作后， 头节点自动弹出队列，如果该对象未能完成处理， 可以调用 fque_revert 将节点归还给队列并置于队头
 *   */
extern
struct tx_node *fque_get(struct tx_fifo *fque);

extern
int fque_size(struct tx_fifo *fque);

#endif