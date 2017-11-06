#ifndef FQUE_H_20170118
#define FQUE_H_20170118

/* forward queued */

#include <netinet/in.h>
#include <stdlib.h>
#include <memory.h>

#include "clist.h"
#include "posix_thread.h"

struct tx_node {
    unsigned char *data; /* ����д������ݻ�����ԭʼָ�� */
    int wcb; /* �ܹ���д���С */
    int offset; /* ��ǰ�����д��ƫ�� */
    struct sockaddr_in udp_target; /* UDP ���ã� ָ������Ŀ�� */
    struct list_head link; /* ���� */
}  ;

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
extern
int fque_push(struct tx_fifo *fque, unsigned char *data, int cb, const struct sockaddr_in *target); /* ע�⣬ �ڲ��������������, ��ָ����뱣֤�Ƕ��ڴ�*/

/* 
 * (δ��ɲ����Ľڵ�)�黹�� FIFO, ���ҷ����ڶ���ͷ
 * ���ִ���ʽ�� ������Դ��ͷ����δ�����Ƴ�������ɲ���
 *  */
extern
int fque_revert(struct tx_fifo *fque, struct tx_node *node);

/* 
 * ȡ������ͷ���� �������Ϊ�գ� ����NULL
 * ������ ͷ�ڵ��Զ��������У�����ö���δ����ɴ��� ���Ե��� fque_revert ���ڵ�黹�����в����ڶ�ͷ
 *   */
extern
struct tx_node *fque_get(struct tx_fifo *fque);

extern
int fque_size(struct tx_fifo *fque);

#endif