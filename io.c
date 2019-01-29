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
    int divisions;		/* count of epoll thread */
};

static struct epoll_object_manager epmgr;

static void __iorun(struct epoll_event *evts, int sigcnt) {
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
            objclos(hld);
            continue;
        }

    	/* concern but not deal with EPOLLHUP 
    	 * every connect request should trigger a EPOLLHUP event, no matter successful or failed*/
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

static void *__epoll_proc(void *argv) {
    struct epoll_event evts[EPOLL_SIZE];
    int sigcnt;
    int errcode;
    struct epoll_object *epo;
    static const int EP_TIMEDOUT = -1;

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

int __ioinit() {
    int i;

    /* write a closed socket, return EPIPE and raise a SIGPIPE signal. ignore it */
    signal(SIGPIPE, SIG_IGN);
    
    epmgr.divisions = posix__getnprocs();
    if ( NULL == (epmgr.epos = (struct epoll_object *)malloc(sizeof(struct epoll_object) * epmgr.divisions))) {
        return -1;
    }
    
    for (i = 0; i < epmgr.divisions; i++) {
        epmgr.epos[i].load = 0;
        epmgr.epos[i].epfd = epoll_create(EPOLL_SIZE);
        if (epmgr.epos[i].epfd < 0) {
            nis_call_ecr("[nshost.io.epoll] fatal error occurred syscall epoll_create(2), error:%d", errno);
            epmgr.epos[i].actived = 0;
            continue;
        }
        
        /* active field as a judge of operational effectiveness, as well as a control symbol of operation  */
        epmgr.epos[i].actived = 1;
        if (posix__pthread_create(&epmgr.epos[i].thread, &__epoll_proc, &epmgr.epos[i]) < 0) {
            nis_call_ecr("[nshost.io.epoll] fatal error occurred syscall pthread_create(3), error:%d", errno);
            epmgr.epos[i].actived = 0;
            close(epmgr.epos[i].epfd);
            epmgr.epos[i].epfd = -1;
        }
    }

    return 0;
}

posix__atomic_initial_declare_variable(__inited__);

int ioinit() {
    if (posix__atomic_initial_try(&__inited__)) {
        if (__ioinit() < 0) {
            posix__atomic_initial_exception(&__inited__);
        } else {
            posix__atomic_initial_complete(&__inited__);
        }
    }

    return __inited__;
}

void iouninit() {
    int i;

    if (!posix__atomic_initial_regress(__inited__)) {
        return;
    }
    
    if (!epmgr.epos) {
        return;
    }
    
    for (i = 0; i < epmgr.divisions; i++){
        if (epmgr.epos[i].epfd > 0){
            close(epmgr.epos[i].epfd);
            epmgr.epos[i].epfd = -1;
        }
        
        if (epmgr.epos[i].actived){
           posix__atomic_xchange(&epmgr.epos[i].actived, 0);
           posix__pthread_join(&epmgr.epos[i].thread, NULL);
        }
    }
    
    free(epmgr.epos);
    epmgr.epos = NULL;
}

int ioatth(void *ncbptr, int mask) {
    struct epoll_event e_evt;
    ncb_t *ncb;

    if (!posix__atomic_initial_passed(__inited__)) {
        return -1;
    }
    
    ncb = (ncb_t *)ncbptr;
    if (!ncb) {
        return -EINVAL;
    }
    
    memset(&e_evt, 0, sizeof(e_evt));
    e_evt.data.u64 = (uint64_t)ncb->hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR); 
	e_evt.events |= mask;
	
	ncb->epfd = epmgr.epos[ncb->hld % epmgr.divisions].epfd;
    if ( epoll_ctl(ncb->epfd, EPOLL_CTL_ADD, ncb->sockfd, &e_evt) < 0 &&
            errno != EEXIST ) {
        nis_call_ecr("[nshost.io.ioatth] fatal error occurred syscall epoll_ctl(2) when add sockfd:%d upon epollfd:%d with mask:%d, error:%u,link:%lld", 
            ncb->sockfd, ncb->epfd, mask, errno, ncb->hld);
        ncb->epfd = -1;
        return -1;
	}

    nis_call_ecr("[nshost.io.ioatth] success associate sockfd:%d with epfd:%d, link:%lld", ncb->sockfd, ncb->epfd, ncb->hld);
	return 0;
}

int iomod(void *ncbptr, int mask ) {
    struct epoll_event e_evt;
    ncb_t *ncb;

    if (!posix__atomic_initial_passed(__inited__)) {
        return -1;
    }
    
    ncb = (ncb_t *)ncbptr;
    if (!ncb) {
        return -EINVAL;
    }

    e_evt.data.u64 = (uint64_t)ncb->hld;
    e_evt.events = (EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR); 
	e_evt.events |= mask;
	
    if ( epoll_ctl(ncb->epfd, EPOLL_CTL_MOD, ncb->sockfd, &e_evt) < 0 ) {
        nis_call_ecr("[nshost.io.ctlmod] fatal error occurred syscall epoll_ctl(2) when modify sockfd:%d upon epollfd:%d with mask:%d, error:%u, link:%lld", 
            ncb->sockfd, ncb->epfd, mask, errno, ncb->hld);
        return -1;
    }

    return 0;
}

void iodeth(void *ncbptr) {
    struct epoll_event evt;
    ncb_t *ncb;

    if (posix__atomic_initial_passed(__inited__)) {
        ncb = (ncb_t *)ncbptr;
        if (ncb) {
            if (epoll_ctl(ncb->epfd, EPOLL_CTL_DEL, ncb->sockfd, &evt) < 0) {
                nis_call_ecr("[nshost.io.iodeth] fatal error occurred syscall epoll_ctl(2) when remove sockfd:%d from epollfd:%d, error:%u, link:%lld", 
                    ncb->sockfd, ncb->epfd, errno, ncb->hld);
            }
        }
    }
}

void ioclose(void *ncbptr) {
    ncb_t *ncb = (ncb_t *)ncbptr;
    if (!ncb){
        return;
    }
    
    if (ncb->sockfd > 0){

        /* It is necessary to ensure that the SOCKET descriptor is removed from the EPOLL before closing the SOCKET,
           otherwise the epoll_wait function has a thread security problem and the behavior is not defined.

           While one thread is blocked in a call to epoll_pwait(), 
           it is possible for another thread to add a file descriptor to the waited-upon epoll instance. 
           If the new file descriptor becomes ready, it will cause the epoll_wait() call to unblock. 
            For a discussion of what may happen if a file descriptor in an epoll instance being monitored by epoll_wait() is closed in another thread, see select(2) 

            If a file descriptor being monitored by select() is closed in another thread, 
            the result is unspecified. On some UNIX systems, select() unblocks and returns, 
            with an indication that the file descriptor is ready (a subsequent I/O operation will likely fail with an error, 
            unless another the file descriptor reopened between the time select() returned and the I/O operations was performed). 
            On Linux (and some other systems), closing the file descriptor in another thread has no effect on select().
            In summary, any application that relies on a particular behavior in this scenario must be considered buggy 
        */
        if (ncb->epfd > 0){
            iodeth(ncb);
            ncb->epfd = -1;
        }
         
        shutdown(ncb->sockfd, SHUT_RDWR);
        close(ncb->sockfd);
        ncb->sockfd = -1;
    }
}

int setasio(int fd) {
    int opt;

    if (fd < 0) {
        return -1;
    }

    opt = fcntl(fd, F_GETFL);
    if (opt < 0) {
        nis_call_ecr("[nshost.io.setasio] fatal error occurred syscall fcntl(2) with F_GETFL.error:%d", errno);
        return -1;
    }

    if (fcntl(fd, F_SETFL, opt | O_NONBLOCK) < 0) {
        nis_call_ecr("[nshost.io.setasio] fatal error occurred syscall fcntl(2) with F_SETFL.error:%d", errno);
        return -1;
    }

    opt = fcntl(fd, F_GETFD);
    if (opt < 0) {
        nis_call_ecr("[nshost.io.setasio] fatal error occurred syscall fcntl(2) with F_GETFD.error:%d", errno);
        return -1;
    }

    /* to disable the port inherit when fork/exec */
    if (fcntl(fd, F_SETFD, opt | FD_CLOEXEC) < 0) {
        nis_call_ecr("[nshost.io.setasio] fatal error occurred syscall fcntl(2) with F_SETFD.error:%d", errno);
        return -1;
    }

    return 0;
}