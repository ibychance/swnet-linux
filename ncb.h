#if !defined NETCONTROLBLOCK
#define NETCONTROLBLOCK

#include "compiler.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <sys/socket.h>
#include <sys/epoll.h>

#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "nis.h"
#include "clist.h"
#include "object.h"
#include "posix_thread.h"

enum ncb__protocol_type {
    kProtocolType_Unknown = 0,
    kProtocolType_TCP,
    kProtocolType_UDP,
};

struct tx_fifo {
    int blocking;
    int size;
    posix__pthread_mutex_t lock;
    struct list_head head;
};

struct _ncb;
typedef int (*ncb_routine_t)(struct _ncb *);

struct _ncb {
    objhld_t hld;       /* the object handle of this ncb */
    int sockfd;         /* the file-descriptor of socket */
    int epfd;           /* the file-descriptor of epoll which binding with @sockfd */
    enum ncb__protocol_type protocol;
    struct list_head nl_entry;      /* the link entry of all ncb object */

    /* the actually buffer for receive */
    unsigned char *packet;

    /* fifo queue of pending packet for send */
    struct tx_fifo fifo;

    /* local/remote address information */
    struct sockaddr_in remot_addr;
    struct sockaddr_in local_addr;

    /* the user-specified nshost event handler */
    nis_callback_t nis_callback;

    /* IO response routine */
    ncb_routine_t ncb_read;
    ncb_routine_t ncb_write;
    ncb_routine_t ncb_error;

    /* save the timeout information/options */
    struct timeval rcvtimeo;
    struct timeval sndtimeo;

    /* tos item in IP-head
     * Differentiated Services Field: Dirrerentiated Services Codepoint/Explicit Congestion Not fication
     *  */
    int iptos;

    /* user definition context pointer */
    void *context;
    void *prcontext;
    /* the attributes of TCP link */
    int attr;

    union {
        struct {
            /* TCP packet user-parse offset */
            int rx_parse_offset;

            /* the actually buffer give to syscall @recv */
            unsigned char *rx_buffer;

            /* the large-block information(TCP packets larger than 0x11000B but less than 50MB) */
            unsigned char* lbdata;   /* large-block data buffer */
            int lboffset;   /* save offset in @lbdata */
            int lbsize;     /* the total length include protocol-head */

             /* template for make/build package */
            tst_t template;
            tst_t prtemplate;

            /* MSS of tcp link */
            int mss;
        } tcp;

        struct {
            /* mreq object for IP multicast */
            struct ip_mreq *mreq;

            /* object flags for UDP link */
            int flag;
        } udp;
    } u;
};
typedef struct _ncb ncb_t;

#define ncb_lb_marked(ncb) ((ncb) ? ((NULL != ncb->u.tcp.lbdata) && (ncb->u.tcp.lbsize > 0)) : (0))

extern
void ncb_uninit(int protocol);
extern
int ncb_allocator(void *udata, const void *ctx, int ctxcb);
extern
void ncb_destructor(objhld_t ignore, void */*ncb_t * */ncb);

extern
int ncb_set_rcvtimeo(const ncb_t *ncb, const struct timeval *timeo);
extern
int ncb_get_rcvtimeo(const ncb_t *ncb);
extern
int ncb_set_sndtimeo(const ncb_t *ncb, const struct timeval *timeo);
extern
int ncb_get_sndtimeo(const ncb_t *ncb);

extern
int ncb_set_iptos(const ncb_t *ncb, int tos);
extern
int ncb_get_iptos(const ncb_t *ncb);

extern
int ncb_set_window_size(const ncb_t *ncb, int dir, int size);
extern
int ncb_get_window_size(const ncb_t *ncb, int dir, int *size);

extern
int ncb_set_linger(const ncb_t *ncb, int onoff, int lin);
extern
int ncb_get_linger(const ncb_t *ncb, int *onoff, int *lin);

extern
void ncb_post_recvdata(const ncb_t *ncb,  int cb, const unsigned char *data);
extern
void ncb_post_accepted(const ncb_t *ncb, HTCPLINK link);
extern
void ncb_post_connected(const ncb_t *ncb);

#endif
