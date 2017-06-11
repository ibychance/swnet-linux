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