#if !defined KE_H_20170118
#define KE_H_20170118

#include <stdint.h>

#include "object.h"

/*
 * 1. nshost 的核心部分（线程结构）描述
 * 2. CPU调度，内核激活等
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
int pthread_manager_init(int rthcnt, int wthcnt);
extern
void pthread_manager_uninit();
extern
int post_task(objhld_t hld, enum task_type_t ttype);

#endif