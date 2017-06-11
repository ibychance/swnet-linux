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
int io_init();
extern
void io_uninit();
extern
int io_attach(int fd, int hld);
extern
int io_detach(int fd);
extern
int io_raise_asio(int fd);
extern
int io_readonly(void *ncbptr,int hld); 
extern
int io_rdwr(void *ncbptr, int hld);

#endif /* IO_H */