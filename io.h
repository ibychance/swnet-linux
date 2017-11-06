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