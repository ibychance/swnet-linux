#ifndef IO_H_20170118
#define IO_H_20170118

/*
 *  内核 IO 事件及其内部调度
 *  主要涉及 EPOLL 及其关注事件的发布和通知
 *  neo.anderson 2017-01-18
 * 
 *  7.3.0 版本起， 严格遵循 ISR-BH 的原则， EPOLL 事件响应例程无限缩短， 并且不介意牺牲部分性能用于内存拷贝 
 */

#include <sys/epoll.h>

extern
int ioinit();
extern
void iouninit();
extern
int ioatth(void *ncbptr, int mask);
extern
int iomod(void *ncbptr, int mask );
extern
void iodeth(void *ncbptr);
extern
void ioclose(void *ncbptr);
extern
int setasio(int fd);
extern
int setsyio(int fd);

#endif /* IO_H */