#if !defined KE_H_20170118
#define KE_H_20170118

#include <stdint.h>

#include "object.h"
#include "clist.h"

/*
 * 1. nshost �ĺ��Ĳ��֣��߳̽ṹ������
 * 2. CPU���ȣ��ں˼����
 * neo.anderson 2017-01-18
 */

enum task_type {
    kTaskType_Unknown = 0,
    kTaskType_RxOrder,
    kTaskType_RxTest,
    kTaskType_TxOrder,
    kTaskType_TxTest,           /* ������д�룬 ��io blocked ʱ���������޷����, ֻҪ��д���зǿգ� ����Ͷ��һ�� TxTest �������߳�������һ�μ�� */ 
    kTaskType_Destroy,
};

extern
int write_pool_init();
extern
void write_pool_uninit();
extern
int post_write_task(objhld_t hld, enum task_type type);

#endif