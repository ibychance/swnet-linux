#ifndef IO_H_20170118
#define IO_H_20170118

/*
 *  Kernel IO Events and internal scheduling
 *  related to EPOLL publication and notification of its concerns
 *  neo.anderson 2017-01-18
 */

extern
int io_init_tcp();
extern
int io_init_udp();
extern
void io_uninit_tcp();
extern
void io_uninit_udp();
extern
int io_attach(void *ncbptr, int mask);
extern
int io_modify(void *ncbptr, int mask );
extern
void io_detach(void *ncbptr);
extern
void io_close(void *ncbptr);
extern
int io_set_asynchronous(int fd);

#endif /* IO_H */
