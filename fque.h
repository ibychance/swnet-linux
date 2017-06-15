#ifndef FQUE_H_20170118
#define FQUE_H_20170118

/* forward queued */

#include <netinet/in.h>
#include <stdlib.h>
#include <memory.h>

#include "clist.h"
#include "posix_thread.h"

struct packet_node_t {
    unsigned char *data; /* 请求写入的数据缓冲区原始指针 */
    int wcb; /* 总共的写入大小 */
    int offset; /* 当前的完成写入偏移 */
    struct sockaddr_in udp_target; /* UDP 可用， 指定发送目标 */
    struct list_head link; /* 勾链 */
}  ;

#define PACKET_NODE_FREE(node)  \
            do { if (!node) break;if ( node->data ) free(node->data);free(node); } while (0)

struct packet_fifo_t {
    struct list_head head;
    int size;
    posix__pthread_mutex_t lock;
} ;

extern
void fque_init(struct packet_fifo_t *fque);
extern
void fque_uninit(struct packet_fifo_t *fque);


extern
int fque_push(struct packet_fifo_t *fque, unsigned char *data, int cb, const struct sockaddr_in *target); /* 注意， 内部不对数据作深拷贝, 该指针必须保证是堆内存*/

/* 
 * (未完成操作的节点)归还给 FIFO, 并且放置于队列头
 * 这种处理方式， 往往来源于头对象未能在移除周期完成操作
 *  */
extern
int fque_revert(struct packet_fifo_t *fque, struct packet_node_t *node);

/* 
 * 取出队列头对象， 如果队列为空， 返回NULL
 * 操作后， 头节点自动弹出队列，如果该对象未能完成处理， 可以调用 fque_revert 将节点归还给队列并置于队头
 *   */
extern
struct packet_node_t *fque_get(struct packet_fifo_t *fque);

extern
int fque_size(struct packet_fifo_t *fque);

#endif