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

extern
int ioinit();
extern
void iouninit();
extern
int ioatth(int fd, int hld);
extern
int iodeth(int fd);
extern
int io_raise_asio(int fd);
extern
int iordonly(void *ncbptr,int hld); 
extern
int iordwr(void *ncbptr, int hld);

#endif /* IO_H */