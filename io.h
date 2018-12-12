#ifndef IO_H_20170118
#define IO_H_20170118

/*
 *  Kernel IO Events and internal scheduling 
 *  related to EPOLL publication and notification of its concerns 
 *  neo.anderson 2017-01-18
 */

extern
int ioinit();
extern
void iouninit();
extern
int ioatth(const void *ncbptr, int mask);
extern
int iomod(const void *ncbptr, int mask );
extern
void iodeth(const void *ncbptr);
extern
void ioclose(void *ncbptr);
extern
int setasio(int fd);
extern
int setsyio(int fd);

#endif /* IO_H */