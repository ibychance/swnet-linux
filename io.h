#ifndef IO_H_20170118
#define IO_H_20170118

/*
 *  �ں� IO �¼������ڲ�����
 *  ��Ҫ�漰 EPOLL �����ע�¼��ķ�����֪ͨ
 *  neo.anderson 2017-01-18
 * 
 *  7.3.0 �汾�� �ϸ���ѭ ISR-BH ��ԭ�� EPOLL �¼���Ӧ�����������̣� ���Ҳ����������������������ڴ濽�� 
 */

#include <sys/epoll.h>

enum io_poll_mask_t {
    kPollMask_Oneshot = 1,          // ����epoll���� EPOLLONESHOT
    kPollMask_Read = 2,             // ����epoll��ע EPOLLIN
    kPollMask_Write = 4,            // ����epoll��ע EPOLLOUT
};

extern
int ioinit();
extern
void iouninit();
extern
int ioatth(void *ncbptr, enum io_poll_mask_t mask);
extern
int iomod(void *ncbptr, enum io_poll_mask_t mask );
extern
int iodeth(int fd);
extern
int setasio(int fd);
extern
int setsyio(int fd);

#endif /* IO_H */