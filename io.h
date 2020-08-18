#ifndef IO_H_20170118
#define IO_H_20170118

/*
 *  Kernel IO Events and internal scheduling
 *  related to EPOLL publication and notification of its concerns
 *  Jerry.Anderson 2017-01-18
 */

extern
int io_init(int protocol);
extern
int io_fcntl(int fd);
extern
void io_uninit(int protocol);
extern
int io_attach(void *ncbptr, int mask);
extern
int io_modify(void *ncbptr, int mask );
extern
void io_detach(void *ncbptr);
extern
void io_close(void *ncbptr);
extern
int io_pipefd(void *ncbptr);

#endif /* IO_H */
