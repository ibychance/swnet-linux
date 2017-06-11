#ifndef FQUE_H_20170118
#define FQUE_H_20170118

/* forward queued */

#include <netinet/in.h>
#include <stdlib.h>
#include <memory.h>

#include "clist.h"
#include "posix_thread.h"

typedef struct _pktnode {
    unsigned char *packet_; /* ����д������ݻ�����ԭʼָ�� */
    int wcb_; /* �ܹ���д���С */
    int offset_; /* ��ǰ�����д��ƫ�� */
    struct sockaddr_in target_; /* UDP ���ã� ָ������Ŀ�� */
    struct list_head link_; /* ���� */
} packet_node_t;

#define PACKET_NODE_FREE(node)  \
            do { if (!node) break;if ( node->packet_ ) free(node->packet_);free(node); } while (0)

typedef struct {
    struct list_head head_;
    int size_;
    posix__pthread_mutex_t lock_;
} packet_fifo_t;

extern
void fque_init(packet_fifo_t *fque);
extern
void fque_uninit(packet_fifo_t *fque);


extern
int fque_push(packet_fifo_t *fque, unsigned char *data, int cb, const struct sockaddr_in *target); /* ע�⣬ �ڲ��������������, ��ָ����뱣֤�Ƕ��ڴ�*/

/* 
 * (δ��ɲ����Ľڵ�)�黹�� FIFO, ���ҷ����ڶ���ͷ
 * ���ִ���ʽ�� ������Դ��ͷ����δ�����Ƴ�������ɲ���
 *  */
extern
int fque_revert(packet_fifo_t *fque, packet_node_t *node);

/* 
 * ȡ������ͷ���� �������Ϊ�գ� ����NULL
 * ������ ͷ�ڵ��Զ��������У�����ö���δ����ɴ��� ���Ե��� fque_revert ���ڵ�黹�����в����ڶ�ͷ
 *   */
extern
packet_node_t *fque_get(packet_fifo_t *fque);

extern
int fque_size(packet_fifo_t *fque);

#endif