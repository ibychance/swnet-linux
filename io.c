#include "io.h"

#include <fcntl.h>

#include <sys/signal.h>

#include "posix_thread.h"
#include "posix_atomic.h"
#include "posix_ifos.h"

#include "ncb.h"
#include "wpool.h"
#include "mxx.h"

/* 1024 is just a hint for the kernel */
#define EPOLL_SIZE    (1024)

struct epoll_object {
    int epfd;
    posix__boolean_t actived;
    posix__pthread_t thread;
    int load; /* load of current thread */
} ;

struct epoll_object_manager {
    struct epoll_object *epos;
    struct epoll_object *epos2; /* the epoll for UDP object */
    int divisions;		/* count of epoll thread */
};

static struct epoll_object_manager epmgr = {NULL, NULL, 0};

static void __iorun(struct epoll_event *evts, int sigcnt)
{
    int i;
    ncb_t *ncb;
    objhld_t hld;

    for (i = 0; i < sigcnt; i++) {
        hld = (objhld_t)evts[i].data.u64;

        /* disconnect/error happend */
        if (evts[i].events & EPOLLRDHUP) {
            nis_call_ecr("[nshost.io.__iorun] event EPOLLRDHUP on link:%lld", hld);
	        objclos(hld);
            continue;
        }

        ncb = (ncb_t *)objrefr(hld);
        if (!ncb) {
            continue;
        }

        if (evts[i].events & EPOLLERR) {
            if (ncb->ncb_error) {
                ncb->ncb_error(ncb);
            }
            nis_call_ecr("[nshost.io.__iorun] event EPOLLERR on link:%lld", hld);
            objdefr(hld);
            objclos(hld);
            continue;
        }

    	/* concern but not deal with EPOLLHUP
    	 * every connect request should trigger a EPOLLHUP event, no matter successful or failed
         *   EPOLLHUP
         *    Hang up happened on the associated file descriptor.  epoll_wait(2) will always wait for this event; it is not necessary to set it in events.
         *
         *    Note  that  when  reading from a channel such as a pipe or a stream socket, this event merely indicates that the peer closed its end of the channel.
         *    Subsequent reads from the channel will return 0 (end of file) only after all outstanding data in
         *    the channel has been consumed. */
    	if ( evts[i].events & EPOLLHUP ) {
    	    ;
    	}

        /* IN event by epoll */
        if (evts[i].events & EPOLLIN) {
            if (ncb->ncb_read) {
                if (ncb->ncb_read(ncb) < 0) {
                    nis_call_ecr("[nshost.io.__iorun] ncb read function return fatal error, this will cause link close, link:%lld", hld);
                    objclos(ncb->hld);
                }
            }else{
                nis_call_ecr("[nshost.io.__iorun] ncb read function unspecified,link:%lld", hld);
            }
        }

        /* OUT event by epoll */
        if (evts[i].events & EPOLLOUT) {
            wp_queued(ncb->hld);
        }

        objdefr(hld);
    }
}

static void *__epoll_proc(void *argv)
{
    struct epoll_event evts[EPOLL_SIZE];
    int sigcnt;
    int errcode;
    struct epoll_object *epo;
    static const int EP_TIMEDOUT = 5000;

    epo = (struct epoll_object *)argv;
    nis_call_ecr("[nshost.io.epoll] epfd:%d LWP:%u startup.", epo->epfd, posix__gettid());

    while (epo->actived) {
        sigcnt = epoll_wait(epo->epfd, evts, EPOLL_SIZE, EP_TIMEDOUT);
        if (sigcnt < 0) {
            errcode = errno;

	    /* The call was interrupted by a signal handler before either :
	     * (1) any of the requested events occurred or
	     * (2) the timeout expired; */
            if (EINTR == errcode) {
                continue;
            }

            nis_call_ecr("[nshost.io.epoll] fatal error occurred syscall epoll_wait(2), epfd:%d, LWP:%u, error:%d", epo->epfd, posix__gettid(), errcode);
            break;
        }

        /* at least one signal is awakened,
            otherwise, timeout trigger. */
        if (sigcnt > 0) {
            __iorun(evts, sigcnt);
        }
    }

    nis_call_ecr("[nshost.io.epoll] epfd:%d LWP:%u terminated.", epo->epfd, posix__gettid());
    posix__pthread_exit( (void *)0 );
    return NULL;
}

static int __ioinit(struct epoll_object *epo, int divisions)
{
    int i;

    if (!epo || divisions <= 0) {
        return -EINVAL;
    }

    for (i = 0; i < divisions; i++) {
        epo[i].load = 0;
        epo[i].epfd = epoll_create(EPOLL_SIZE);
        if (epo[i].epfd < 0) {
            nis_call_ecr("[nshost.io.__ioinit] fatal error occurred syscall epoll_create(2), error:%d", errno);
            epo[i].actived = 0;
            continue;
        }

        /* active field as a judge of operational effectiveness, as well as a control symbol of operation  */
        epo[i].actived = 1;
        if (posix__pthread_create(&epo[i].thread, &__epoll_proc, &epo[i]) < 0) {
            nis_call_ecr("[nshost.io.__ioinit] fatal error occurred syscall pthread_create(3), error:%d", errno);
            epo[i].actived = 0;
            close(epo[i].epfd);
            epo[i].epfd = -1;
        }
    }

    return 0;
}

posix__atomic_initial_declare_variable(__io_inited_global__);
static int __io_init_global()
{
    int retval;

    if (posix__atomic_initial_try(&__io_inited_global__)) {
        epmgr.divisions = posix__getnprocs();

        /* write a closed socket, return EPIPE and raise a SIGPIPE signal. ignore it */
        signal(SIGPIPE, SIG_IGN);

        retval = 0;
        do {
            if ( NULL == (epmgr.epos = (struct epoll_object *)malloc(sizeof(struct epoll_object) * epmgr.divisions))) {
                retval = -ENOMEM;
                break;
            }
            memset(epmgr.epos, 0, sizeof(struct epoll_object));

            if ( NULL == (epmgr.epos2 = (struct epoll_object *)malloc(sizeof(struct epoll_object) * epmgr.divisions))) {
                retval = -ENOMEM;
                break;
            }
            memset(epmgr.epos, 0, sizeof(struct epoll_object));

        } while(0);

        if (0 == retval ){
            posix__atomic_initial_complete(&__io_inited_global__);
        } else {
            posix__atomic_initial_exception(&__io_inited_global__);
        }
    }

    return __io_inited_global__;
}

posix__atomic_initial_declare_variable(__io_inited_tcp__);
int io_init_tcp()
{
    int retval;

    retval = __io_init_global();
    if (retval < 0) {
        return retval;
    }

    if (posix__atomic_initial_try(&__io_inited_tcp__)) {
        if (__ioinit(epmgr.epos, epmgr.divisions) < 0) {
            posix__atomic_initial_exception(&__io_inited_tcp__);
        } else {
            posix__atomic_initial_complete(&__io_inited_tcp__);
        }
    }

    return __io_inited_tcp__;
}

posix__atomic_initial_declare_variable(__io_inited_udp__);
int io_init_udp()
{
    if (__io_init_global() < 0) {
        return -1;
    }

    if (posix__atomic_initial_try(&__io_inited_udp__)) {

        if (0 == epmgr.divisions) {
            epmgr.divisions = posix__getnprocs();

            /* write a closed socket, return EPIPE and raise a SIGPIPE signal. ignore it */
            signal(SIGPIPE, SIG_IGN);
        }



        if (__ioinit(epmgr.epos2, epmgr.divisions) < 0) {
            posix__atomic_initial_exception(&__io_inited_udp__);
        } else {
            posix__atomic_initial_complete(&__io_inited_udp__);
        }
    }

    return __io_inited_udp__;
}

static void __iouninit(struct epoll_object *epo, int division)
{
    int i;

    if (!epo || division <= 0) {
        return;
    }

    for (i = 0; i < division; i++){
        if (epo[i].epfd > 0){
            close(epo[i].epfd);
            epo[i].epfd = -1;
        }

        if (epo[i].actived){
           posix__atomic_xchange(&epo[i].actived, 0);
           posix__pthread_join(&epo[i].thread, NULL);
        }
    }

    free(epo);
}

void io_uninit_tcp()
{
    if (!posix__atomic_initial_regress(&__io_inited_tcp__)) {
        return;
    }

    if (!epmgr.epos) {
        return;
    }

    __iouninit(epmgr.epos, epmgr.divisions);
    epmgr.epos = NULL;
    epmgr.divisions = 0;
}

void io_uninit_udp()
{
    if (!posix__atomic_initial_regress(&__io_inited_udp__)) {
        return;
    }

    if (!epmgr.epos2) {
        return;
    }

    __iouninit(epmgr.epos2, epmgr.divisions);
    epmgr.epos2 = NULL;
    epmgr.divisions = 0;
}

/* boolean return */
static int io_inner_check(ncb_t *ncb) 
{
	if (ncb->proto_type == kProtocolType_TCP) {
		return posix__atomic_initial_passed(__io_inited_tcp__);
    }
	
	if (ncb->proto_type == kProtocolType_UDP) {
		return posix__atomic_initial_passed(__io_inited_udp__);
    }
	
	return 0;
}

int io_attach(void *ncbptr, int mask)
{
    struct epoll_event e_evt;
    ncb_t *ncb;
    struct epoll_object *epo;

    ncb = (ncb_t *)ncbptr;
    if (!ncb) {
        return -EINVAL;
    }

	epo = NULL;
    if (ncb->proto_type == kProtocolType_TCP) {
		if (posix__atomic_initial_passed(__io_inited_tcp__)) {
			epo = epmgr.epos;
		}
    }
	
	if (ncb->proto_type == kProtocolType_UDP) {
		if (posix__atomic_initial_passed(__io_inited_udp__)) {
			epo = epmgr.epos2;
		}
    }

    if (!epo) {
        nis_call_ecr("[nshost.io.io_attach] unknown protocol type:%d specified in ncb", ncb->proto_type);
        return -EPROTOTYPE;
    }

    memset(&e_evt, 0, sizeof(e_evt));
    e_evt.data.u64 = (uint64_t)ncb->hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR);
	e_evt.events |= mask;

	ncb->epfd = epo[ncb->hld % epmgr.divisions].epfd;
    if ( epoll_ctl(ncb->epfd, EPOLL_CTL_ADD, ncb->sockfd, &e_evt) < 0 &&
            errno != EEXIST ) {
        nis_call_ecr("[nshost.io.io_attach] fatal error occurred syscall epoll_ctl(2) when add sockfd:%d upon epollfd:%d with mask:%d, error:%u,link:%lld",
            ncb->sockfd, ncb->epfd, mask, errno, ncb->hld);
        ncb->epfd = -1;
        return -1;
	}

    nis_call_ecr("[nshost.io.io_attach] success associate sockfd:%d with epfd:%d, link:%lld", ncb->sockfd, ncb->epfd, ncb->hld);
	return 0;
}

int io_modify(void *ncbptr, int mask )
{
    struct epoll_event e_evt;
    ncb_t *ncb;

    ncb = (ncb_t *)ncbptr;
    if (!ncb) {
        return -EINVAL;
    }
	
	if (!io_inner_check(ncb)) {
		return -EINVAL;
	}

    e_evt.data.u64 = (uint64_t)ncb->hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR);
	e_evt.events |= mask;

    if ( epoll_ctl(ncb->epfd, EPOLL_CTL_MOD, ncb->sockfd, &e_evt) < 0 ) {
        nis_call_ecr("[nshost.io.io_modify] fatal error occurred syscall epoll_ctl(2) when modify sockfd:%d upon epollfd:%d with mask:%d, error:%u, link:%lld",
            ncb->sockfd, ncb->epfd, mask, errno, ncb->hld);
        return -1;
    }

    return 0;
}

void io_detach(void *ncbptr)
{
    struct epoll_event evt;
    ncb_t *ncb;

    ncb = (ncb_t *)ncbptr;
    if (ncb) {
		if (!io_inner_check(ncb)) {
			return;
		}
		
        if (epoll_ctl(ncb->epfd, EPOLL_CTL_DEL, ncb->sockfd, &evt) < 0) {
            nis_call_ecr("[nshost.io.io_detach] fatal error occurred syscall epoll_ctl(2) when remove sockfd:%d from epollfd:%d, error:%u, link:%lld",
                ncb->sockfd, ncb->epfd, errno, ncb->hld);
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

int io_set_asynchronous(int fd)
{
    int opt;

    if (fd < 0) {
        return -1;
    }

    opt = fcntl(fd, F_GETFL);
    if (opt < 0) {
        nis_call_ecr("[nshost.io.io_set_asynchronous] fatal error occurred syscall fcntl(2) with F_GETFL.error:%d", errno);
        return -1;
    }

    if (fcntl(fd, F_SETFL, opt | O_NONBLOCK) < 0) {
        nis_call_ecr("[nshost.io.io_set_asynchronous] fatal error occurred syscall fcntl(2) with F_SETFL.error:%d", errno);
        return -1;
    }

    opt = fcntl(fd, F_GETFD);
    if (opt < 0) {
        nis_call_ecr("[nshost.io.io_set_asynchronous] fatal error occurred syscall fcntl(2) with F_GETFD.error:%d", errno);
        return -1;
    }

    /* to disable the port inherit when fork/exec */
    if (fcntl(fd, F_SETFD, opt | FD_CLOEXEC) < 0) {
        nis_call_ecr("[nshost.io.io_set_asynchronous] fatal error occurred syscall fcntl(2) with F_SETFD.error:%d", errno);
        return -1;
    }

    return 0;
}
