#include "pipe.h"

#include <fcntl.h>
#include <limits.h>

#include "mxx.h"
#include "posix_atomic.h"
#include "io.h"

struct pipe_package_head
{
	int length;
	objhld_t link;
	unsigned char pipedata[0];
};

static int pipe_initialize(void *udata, const void *ctx, int ctxcb)
{
	return 0;
}

static void pipe_unloader(objhld_t hld, void *udata)
{
	ncb_t *ncb;

	ncb = (ncb_t *)udata;
	if (ncb->sockfd) {
		close(ncb->sockfd);
		ncb->sockfd = 0;
	}
}

static int pipe_rx(ncb_t *ncb)
{
	unsigned char pipebuf[PIPE_BUF];
	int n;
	struct pipe_package_head *pipepkt;
	ncb_t *ncb_link;

	while (1) {
		n = read(ncb->sockfd, pipebuf, sizeof(pipebuf));
		if (n <= 0) {
			if ( (0 == errno) || (EAGAIN == errno) || (EWOULDBLOCK == errno) ) {
				return 0;
			}

			/* system interrupted */
	        if (EINTR == errno) {
	        	continue;
	        }

			return -1;
		}

		if (n < sizeof(struct pipe_package_head)) {
			continue;
		}

		pipepkt = (struct pipe_package_head *)&pipebuf[0];
		if ( n != (pipepkt->length + sizeof(struct pipe_package_head)) ) {
			continue;
		}

		ncb_link = objrefr(pipepkt->link);
		if (ncb_link) {
			ncb_post_pipedata(ncb_link, pipepkt->length, pipepkt->pipedata);
			objdefr(pipepkt->link);
		}
	}

	return 0;
}

static int pipe_tx(ncb_t *ncb)
{
	return 0;
}

int pipe_create(int protocol)
{
	int pipefd[2];
	objhld_t hld;
	ncb_t *ncb;

	if (0 != pipe2(pipefd, O_DIRECT | O_NONBLOCK)) {
		return -1;
	}

    hld = objallo(sizeof(ncb_t), &pipe_initialize, &pipe_unloader, NULL, 0);
    if (hld < 0) {
        nis_call_ecr("[nshost.pipe.create] insufficient resource for allocate inner object");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    ncb = (ncb_t *) objrefr(hld);
    assert(ncb);

    do {
        ncb->nis_callback = NULL;
        /* the read file-descriptor of pipe */
        ncb->sockfd = pipefd[0];
        ncb->hld = hld;
        ncb->protocol = protocol;

        /* set data handler function pointer for Rx/Tx */
        posix__atomic_set(&ncb->ncb_read, &pipe_rx);
        posix__atomic_set(&ncb->ncb_write, &pipe_tx);

        /* attach to epoll */
        if (io_attach(ncb, EPOLLIN) < 0) {
            break;
        }

        objdefr(hld);
        return pipefd[1];
    } while (0);

    close(pipefd[1]);
    objdefr(hld);
    objclos(hld);
    return -1;
}

int pipe_write_message(ncb_t *ncb, const unsigned char *data, int cb)
{
	struct pipe_package_head *pipemsg;
	int pipefd;
	int n;

	if (cb >= (PIPE_BUF - sizeof(struct pipe_package_head)) ) {
		return -EINVAL;
	}

	pipefd = io_pipefd(ncb);
	if (pipefd <= 0 ) {
		return -1;
	}

	n = sizeof(struct pipe_package_head) + cb;
	pipemsg = (struct pipe_package_head *)malloc(n);
	if (!pipemsg) {
		return -ENOMEM;
	}

	memcpy(pipemsg->pipedata, data, cb);
	pipemsg->length = cb;
	pipemsg->link = ncb->hld;

	return write(pipefd, pipemsg, n);
}
