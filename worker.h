#if !defined KE_H_20170118
#define KE_H_20170118

#include <stdint.h>

#include "object.h"

/*
 * 1. nshost �ĺ��Ĳ��֣��߳̽ṹ������
 * 2. CPU���ȣ��ں˼����
 * neo.anderson 2017-01-18
 */

enum task_type_t {
    kTaskType_Unknown = 0,
    kTaskType_Read,
    kTaskType_Write,
    kTaskType_Destroy,
    kTaskType_User,
};

extern
int wtpinit();
extern
void wtpuninit();
extern
int post_task(objhld_t hld, enum task_type_t ttype);

#endif