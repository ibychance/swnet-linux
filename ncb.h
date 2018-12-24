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
} ;

#define MAXIMUM_NCB_FIFO_SIZE       (100)

typedef struct _ncb {
    objhld_t hld;
    int sockfd;
    int epfd;  /* the file-descriptor of epoll */
    enum ncb__protocol_type proto_type;

    /* the actually buffer for receive */
    char *packet;
    
    /* fifo queue of pending packet for send */
    struct tx_fifo fifo;

    /* local/remote address information */
    struct sockaddr_in remot_addr;
    struct sockaddr_in local_addr;

    /* the user-specified nshost event handler */
    nis_callback_t nis_callback;
    
    /* IO response routine */
    int (*ncb_read)(struct _ncb *);
    int (*ncb_write)(struct _ncb *);
    int (*ncb_error)(struct _ncb *);
    
    /* save the timeout information/options */
    struct timeval rcvtimeo;
    struct timeval sndtimeo;
    
    /* tos item in IP-head
     * Differentiated Services Field: Dirrerentiated Services Codepoint/Explicit Congestion Not fication
     *  */
    int iptos;

    /* object attribute */
    int flag;
    
    union {
        struct {
            /* TCP packet user-parse offset */
            int rx_parse_offset;
            
            /* the actually buffer give to syscall @recv */
            char *rx_buffer;
    
            /* the large-block information(TCP packets larger than 0x11000B but less than 50MB) */
            char* lbdata;   /* large-block data buffer */
            int lboffset;   /* save offset in @lbdata */
            int lbsize;     /* the total length include protocol-head */
    
             /* template for make/build package */
            tst_t template;

            /* MSS of tcp link */
            int mss;
        } tcp;
        
        struct {
            /* mreq object for IP multicast */
            struct ip_mreq *mreq;
        } udp;
    } u;
} ncb_t;

#define ncb_lb_marked(ncb) ((ncb) ? ((NULL != ncb->u.tcp.lbdata) && (ncb->u.tcp.lbsize > 0)) : (0))

extern
int ncb_init(ncb_t *ncb);
extern
void ncb_uninit(objhld_t ignore, void */*ncb_t * */ncb);

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
void ncb_post_preclose(const ncb_t *ncb);
extern
void ncb_post_close(const ncb_t *ncb);
extern
void ncb_post_recvdata(const ncb_t *ncb,  int cb, const char *data);
extern
void ncb_post_accepted(const ncb_t *ncb, HTCPLINK link);
extern
void ncb_post_senddata(const ncb_t *ncb,  int cb, const char *data);
extern
void ncb_post_connected(const ncb_t *ncb);

#endif