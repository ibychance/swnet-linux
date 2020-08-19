#include "io.h"

#include <fcntl.h>

#include <sys/signal.h>
#include <signal.h>

#include "posix_thread.h"
#include "posix_atomic.h"
#include "posix_ifos.h"

#include "ncb.h"
#include "wpool.h"
#include "mxx.h"
#include "pipe.h"

/* 1024 is just a hint for the kernel */
#define EPOLL_SIZE    (1024)

struct epoll_object_block {
    int epfd;
    boolean_t actived;
    posix__pthread_t threadfd;
    int load; /* load of current thread */
    int pipefdw;
} ;

struct io_object_block {
    struct epoll_object_block *epoptr;
    int divisions;
    int protocol;
};

static objhld_t tcphld = -1;
static objhld_t udphld = -1;

/*
EPOLL enevts define in and copy form: /usr/include/x86_64-linux-gnu/sys/epoll.h
enum EPOLL_EVENTS
  {
    EPOLLIN = 0x001,
#define EPOLLIN EPOLLIN
    EPOLLPRI = 0x002,
#define EPOLLPRI EPOLLPRI
    EPOLLOUT = 0x004,
#define EPOLLOUT EPOLLOUT
    EPOLLRDNORM = 0x040,
#define EPOLLRDNORM EPOLLRDNORM
    EPOLLRDBAND = 0x080,
#define EPOLLRDBAND EPOLLRDBAND
    EPOLLWRNORM = 0x100,
#define EPOLLWRNORM EPOLLWRNORM
    EPOLLWRBAND = 0x200,
#define EPOLLWRBAND EPOLLWRBAND
    EPOLLMSG = 0x400,
#define EPOLLMSG EPOLLMSG
    EPOLLERR = 0x008,
#define EPOLLERR EPOLLERR
    EPOLLHUP = 0x010,
#define EPOLLHUP EPOLLHUP
    EPOLLRDHUP = 0x2000,
#define EPOLLRDHUP EPOLLRDHUP
    EPOLLEXCLUSIVE = 1u << 28,
#define EPOLLEXCLUSIVE EPOLLEXCLUSIVE
    EPOLLWAKEUP = 1u << 29,
#define EPOLLWAKEUP EPOLLWAKEUP
    EPOLLONESHOT = 1u << 30,
#define EPOLLONESHOT EPOLLONESHOT
    EPOLLET = 1u << 31
#define EPOLLET EPOLLET
  };
*/

static void __iorun(struct epoll_event *evts, int sigcnt)
{
    int i;
    ncb_t *ncb;
    objhld_t hld;
    int (*ncb_read)(struct _ncb *);

    for (i = 0; i < sigcnt; i++) {
        hld = (objhld_t)evts[i].data.u64;

        /* disconnect/error/reset socket states have been detect,
         * in this case, ncb it's NOT necessary */
        if ( (evts[i].events & EPOLLRDHUP) || (evts[i].events & EPOLLERR) ) {
            nis_call_ecr("[nshost.io.__iorun] EPOLL event:%d detect on link:%lld", evts[i].events, hld);
	        objclos(hld);
            continue;
        }

        ncb = (ncb_t *)objrefr(hld);
        if (!ncb) {
            continue;
        }

        /* system width input cache change from empty to readable */
        if (evts[i].events & EPOLLIN) {
            ncb_read = posix__atomic_get(&ncb->ncb_read);
            if (ncb_read) {
                if (ncb_read(ncb) < 0) {
                    nis_call_ecr("[nshost.io.__iorun] ncb read function return fatal error, this will cause link close, link:%lld", hld);
                    objclos(ncb->hld);
                }
            }else{
                nis_call_ecr("[nshost.io.__iorun] ncb read function unspecified,link:%lld", hld);
            }
        }

        /* system width output cache change from full to writeable */
        if (evts[i].events & EPOLLOUT) {

            /* concern but not deal with EPOLLHUP
             * every connect request should trigger a EPOLLHUP event, no matter successful or failed
             * EPOLLHUP
             * Hang up happened on the associated file descriptor.  epoll_wait(2) will always wait for this event; it is not necessary to set it in events.
             *
             * Notes: that when reading from a channel such as a pipe or a stream socket,
             *  this event merely indicates that the peer closed its end of the channel.
             * Subsequent reads from the channel will return 0 (end of file) only after all outstanding data in
             *   the channel has been consumed.
             *
             * EPOLLOUT adn EPOLLHUP for asynchronous connect(2)
             * 1.When the connect function is not called locally, but the socket is attach to epoll for detection,
             *   epoll will generate an EPOLLOUT | EPOLLHUP, that is, an event with a value of 0x14
             * 2.When the local connect event occurs, but the connection fails to be established,
             *   epoll will generate EPOLLIN | EPOLLERR | EPOLLHUP, that is, an event with a value of 0x19
             * 3.When the connect function is also called and the connection is successfully established,
             *   epoll will generate EPOLLOUT once, with a value of 0x4, indicating that the socket is writable
             */
            if ( 0 == (evts[i].events & EPOLLHUP) ) {
                 wp_queued(ncb);
            } else {
                nis_call_ecr("[nshost.io.__iorun] EPOLLOUT with event:%d, link:%lld", evts[i].events, hld);
            }
        }

        objdefr(hld);
    }
}

static void *__epoll_proc(void *argv)
{
    struct epoll_event evts[EPOLL_SIZE];
    int sigcnt;
    struct epoll_object_block *epoptr;
    static const int EP_TIMEDOUT = 100;

    epoptr = (struct epoll_object_block *)argv;
    assert(NULL != epoptr);

    nis_call_ecr("[nshost.io.epoll] epfd:%d LWP:%u startup.", epoptr->epfd, posix__gettid());

    while (YES == epoptr->actived) {
        sigcnt = epoll_wait(epoptr->epfd, evts, EPOLL_SIZE, EP_TIMEDOUT);
        if (sigcnt < 0) {
    	    /* The call was interrupted by a signal handler before either :
    	     * (1) any of the requested events occurred or
    	     * (2) the timeout expired; */
            if (EINTR == errno) {
                continue;
            }

            nis_call_ecr("[nshost.io.epoll] fatal error occurred syscall epoll_wait(2), epfd:%d, LWP:%u, error:%d", epoptr->epfd, posix__gettid(), errno);
            break;
        }

        /* at least one signal is awakened,
            otherwise, timeout trigger. */
        if (sigcnt > 0) {
            __iorun(evts, sigcnt);
        }
    }

    nis_call_ecr("[nshost.io.epoll] epfd:%d LWP:%u terminated.", epoptr->epfd, posix__gettid());
    posix__pthread_exit( (void *)0 );
    return NULL;
}

static int __io_init(struct io_object_block *iobptr, int nprocs)
{
    int i;
    struct epoll_object_block *epoptr;

    if (!iobptr || nprocs <= 0) {
        return -EINVAL;
    }

    iobptr->divisions = nprocs;
    iobptr->epoptr = (struct epoll_object_block *)malloc(sizeof(struct epoll_object_block) * iobptr->divisions);
    if (!iobptr->epoptr) {
        return -ENOMEM;
    }

    for (i = 0; i < iobptr->divisions; i++) {
        epoptr = &iobptr->epoptr[i];
        epoptr->load = 0;
        epoptr->epfd = epoll_create(EPOLL_SIZE); /* kernel don't care about the parameter @size, but request it MUST be large than zero */
        if (epoptr->epfd < 0) {
            nis_call_ecr("[nshost.io.__io_init] fatal error occurred syscall epoll_create(2), error:%d", errno);
            continue;
        }

        /* @actived is the flag for io thread terminate */
        epoptr->actived = YES;
        if (posix__pthread_create(&epoptr->threadfd, &__epoll_proc, epoptr) < 0) {
            nis_call_ecr("[nshost.io.__io_init] fatal error occurred syscall pthread_create(3), error:%d", errno);
            close(epoptr->epfd);
            epoptr->epfd = -1;
            epoptr->actived = NO;
        }
    }

    /* function @io_attach will be invoke during @pipe_create called, so the epoll file-descriptor must create before it */
    for (i = 0; i < iobptr->divisions; i++) {
        epoptr = &iobptr->epoptr[i];
        /* create a pipe object for this thread */
        epoptr->pipefdw = pipe_create(iobptr->protocol);
        if (epoptr->pipefdw < 0) {
            nis_call_ecr("[nshost.io.__io_init] fails create pipe object for epoll threading, error:%d", errno);
        }
    }

    return 0;
}

static void __io_uninit(objhld_t hld, void *udata)
{
    int i;
    struct io_object_block *iobptr;
    struct epoll_object_block *epoptr;

    iobptr = (struct io_object_block *)udata;
    if (!iobptr) {
        return;
    }

    for (i = 0; i < iobptr->divisions; i++) {
        epoptr = &iobptr->epoptr[i];
        if (YES == epoptr->actived) {
            epoptr->actived = NO;
            posix__pthread_join(&epoptr->threadfd, NULL);
        }

        if (epoptr->epfd > 0){
            close(epoptr->epfd);
            epoptr->epfd = -1;
        }
    }

    free(iobptr->epoptr);
    iobptr->epoptr = NULL;

    if (!__sync_bool_compare_and_swap(&tcphld, hld, -1)) {
        __sync_bool_compare_and_swap(&udphld, hld, -1);
    }
}

int io_init(int protocol)
{
    int retval;
    struct io_object_block *iobptr;
    objhld_t hld, *hldptr;
    int nprocs;

    hldptr = ((IPPROTO_TCP ==protocol ) ? &tcphld :
            ((IPPROTO_UDP == protocol || ETH_P_ARP == protocol ) ? &udphld : NULL));
    if (!hldptr) {
        return -EPROTOTYPE;
    }

    if (*hldptr >= 0) {
        return EALREADY;
    }

    hld = objallo(sizeof(struct io_object_block), NULL, &__io_uninit, NULL, 0);
    if (hld < 0) {
        return -1;
    }

    if (! __sync_bool_compare_and_swap(hldptr, -1, hld)) {
        objclos(hld);
        return EALREADY;
    }

    iobptr = objrefr(*hldptr);
    if (!iobptr) {
        return -ENOENT;
    }

    /* less IO threads for UDP business */
    iobptr->protocol = protocol;
    nprocs = (IPPROTO_TCP ==protocol ) ? posix__getnprocs() :
                (posix__getnprocs() >> 1);
    if (nprocs <= 0) {
        nprocs = 1;
    }
    retval = __io_init(iobptr, nprocs);
    objdefr(*hldptr);
    return retval;
}

void io_uninit(int protocol)
{
    objhld_t *hldptr;

    hldptr = ((IPPROTO_TCP ==protocol ) ? &tcphld :
                ((IPPROTO_UDP == protocol ) ? &udphld : NULL));
    if (hldptr) {
        if (*hldptr >= 0) {
            objclos(*hldptr);
        }
    }
}

int io_fcntl(int fd)
{
    int opt;

    if (fd < 0) {
        return -EINVAL;
    }

    opt = fcntl(fd, F_GETFL);
    if (opt < 0) {
        nis_call_ecr("[nshost.io.io_fcntl] fatal error occurred syscall fcntl(2) with F_GETFL.error:%d", errno);
        return posix__makeerror(errno);
    }
    if ( 0 == (opt & O_NONBLOCK )) {
        if (fcntl(fd, F_SETFL, opt | O_NONBLOCK) < 0) {
            nis_call_ecr("[nshost.io.io_fcntl] fatal error occurred syscall fcntl(2) with F_SETFL.error:%d", errno);
            return posix__makeerror(errno);
        }
    }

    opt = fcntl(fd, F_GETFD);
    if (opt < 0) {
        nis_call_ecr("[nshost.io.io_fcntl] fatal error occurred syscall fcntl(2) with F_GETFD.error:%d", errno);
        return posix__makeerror(errno);
    }

    /* to disable the port inherit when fork/exec */
    if (0 == (opt & FD_CLOEXEC)) {
        if (fcntl(fd, F_SETFD, opt | FD_CLOEXEC) < 0) {
            nis_call_ecr("[nshost.io.io_fcntl] fatal error occurred syscall fcntl(2) with F_SETFD.error:%d", errno);
            return posix__makeerror(errno);
        }
    }
    return 0;
}

int io_attach(void *ncbptr, int mask)
{
    objhld_t hld;
    struct io_object_block *iobptr;
    struct epoll_event e_evt;
    int protocol;
    ncb_t *ncb;
    int retval;

    ncb = (ncb_t *)ncbptr;
    assert(ncb);

    retval = io_fcntl(ncb->sockfd);
    if ( retval < 0) {
        return retval;
    }

    protocol = ncb->protocol;
    hld = ((IPPROTO_TCP == protocol ) ? tcphld :
            ((IPPROTO_UDP == protocol || ETH_P_ARP == protocol) ? udphld : -1));
    if (hld < 0) {
        return -EPROTOTYPE;
    }

    iobptr = objrefr(hld);
    if (!iobptr) {
        nis_call_ecr("[nshost.io.io_attach] failed reference assicoated io object block with handle:%lld", hld);
        return -ENOENT;
    }

    memset(&e_evt, 0, sizeof(e_evt));
    e_evt.data.u64 = (uint64_t)ncb->hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR);
	e_evt.events |= mask;

	ncb->epfd = iobptr->epoptr[ncb->hld % iobptr->divisions].epfd;
    if ( epoll_ctl(ncb->epfd, EPOLL_CTL_ADD, ncb->sockfd, &e_evt) < 0 &&
            errno != EEXIST ) {
        nis_call_ecr("[nshost.io.io_attach] fatal error occurred syscall epoll_ctl(2) when add link:%lld with sockfd:%d upon epollfd:%d with mask:%d, error:%u,",
            ncb->hld, ncb->sockfd, ncb->epfd, mask, errno);
        ncb->epfd = -1;
	} else {
        nis_call_ecr("[nshost.io.io_attach] success associate sockfd:%d with epfd:%d, link:%lld", ncb->sockfd, ncb->epfd, ncb->hld);
    }

    objdefr(hld);
	return ncb->epfd;
}

int io_modify(void *ncbptr, int mask )
{
    struct epoll_event e_evt;
    ncb_t *ncb;

    ncb = (ncb_t *)ncbptr;
    if (!ncb) {
        return -EINVAL;
    }

    e_evt.data.u64 = (uint64_t)ncb->hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR);
	e_evt.events |= mask;

    if ( epoll_ctl(ncb->epfd, EPOLL_CTL_MOD, ncb->sockfd, &e_evt) < 0 ) {
        nis_call_ecr("[nshost.io.io_modify] fatal error occurred syscall epoll_ctl(2) when modify link:%lld with sockfd:%d upon epollfd:%d with mask:%d, error:%u, ",
            ncb->hld, ncb->sockfd, ncb->epfd, mask, errno);
        return posix__makeerror(errno);
    }

    return 0;
}

void io_detach(void *ncbptr)
{
    struct epoll_event evt;
    ncb_t *ncb;

    ncb = (ncb_t *)ncbptr;
    if (ncb) {
        if (epoll_ctl(ncb->epfd, EPOLL_CTL_DEL, ncb->sockfd, &evt) < 0) {
            nis_call_ecr("[nshost.io.io_detach] fatal error occurred syscall epoll_ctl(2) when remove link:%lld with sockfd:%d from epollfd:%d, error:%u",
                ncb->hld, ncb->sockfd, ncb->epfd, errno);
        }
    }
}

void io_close(void *ncbptr)
{
    ncb_t *ncb;

    ncb = (ncb_t *)ncbptr;
    if (!ncb){
        return;
    }

    if (ncb->sockfd > 0){

        /* It is necessary to ensure that the SOCKET descriptor is removed from the EPOLL before closing the SOCKET,
           otherwise the epoll_wait function has a thread security problem and the behavior is not defined.

           While one thread is blocked in a call to epoll_pwait(2),
           it is possible for another thread to add a file descriptor to the waited-upon epoll instance.
           If the new file descriptor becomes ready, it will cause the epoll_wait(2) call to unblock.
            For a discussion of what may happen if a file descriptor in an epoll instance being monitored by epoll_wait(2) is closed in another thread, see select(2)

            If a file descriptor being monitored by select(2) is closed in another thread,
            the result is unspecified. On some UNIX systems, select(2) unblocks and returns,
            with an indication that the file descriptor is ready (a subsequent I/O operation will likely fail with an error,
            unless another the file descriptor reopened between the time select(2) returned and the I/O operations was performed).
            On Linux (and some other systems), closing the file descriptor in another thread has no effect on select(2).
            In summary, any application that relies on a particular behavior in this scenario must be considered buggy
        */
        if (ncb->epfd > 0){
            io_detach(ncb);
            ncb->epfd = -1;
        }

        shutdown(ncb->sockfd, SHUT_RDWR);
        close(ncb->sockfd);
        ncb->sockfd = -1;
    }
}

int io_pipefd(void *ncbptr)
{
    ncb_t *ncb;
    objhld_t hld;
    struct io_object_block *iobptr;
    int protocol;
    int pipefd;

    ncb = (ncb_t *)ncbptr;
    assert(ncb);

    protocol = ncb->protocol;
    hld = ((IPPROTO_TCP == protocol ) ? tcphld :
            ((IPPROTO_UDP == protocol || ETH_P_ARP == protocol) ? udphld : -1));
    if (hld < 0) {
        return -EPROTOTYPE;
    }

    iobptr = objrefr(hld);
    if (!iobptr) {
        nis_call_ecr("[nshost.io.io_attach] failed reference assicoated io object block with handle:%lld", hld);
        return -ENOENT;
    }

    pipefd = iobptr->epoptr[ncb->hld % iobptr->divisions].pipefdw;

    objdefr(hld);
    return pipefd;
}
