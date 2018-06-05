#if !defined NETCONTROLBLOCK
#define NETCONTROLBLOCK

#include <errno.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "compiler.h"
#include "posix_thread.h"
#include "posix_atomic.h"

#include "nis.h"
#include "ncb.h"
#include "io.h"
#include "worker.h"
#include "fque.h"
#include "clist.h"
#include "object.h"

enum ncb__protocol_type {
    kProtocolType_Unknown = 0,
    kProtocolType_TCP,
    kProtocolType_UDP,
};

typedef struct _ncb {
    int hld;
    int sockfd;
    int epfd;  /* 绑定的EPOLL描述符 */
    enum ncb__protocol_type proto_type;

    /* 应用层数据包的收包实际缓冲区 */
    char *packet;
    
    /* 发送操作的顺序队列 */
    struct tx_fifo tx_fifo;

    /* 地址结构信息 */
    struct sockaddr_in remot_addr;
    struct sockaddr_in local_addr;

    /* 回调例程 */
    nis_callback_t nis_callback;
    
    /* 用户上下文 */
    char *context;
    int context_size;

    /* IO 响应例程 */
    int (*ncb_read)(struct _ncb *);
    int (*ncb_write)(struct _ncb *);
    
    /* 重要:
     * 用于 write 操作发生 EAGAIN 后
     * 一旦发生 EAGAIN, 后续操作只能通过 EPOLLOUT 触发，具体处理措施为
     * 1. 发生 EAGAIN, 则激活该 FD 的 EPOLLOUT事件，同时将 shield_ 置 1
     * 2. 在 shield_ > 0 的情况下，任何写任务， 均不会真正调用 write
     * 3. EPOLLOUT 事件到达时，将 shield_ 置0， 同时取消 EPOLLOUT 的关注
     * 注意:
     * 1. 一旦发生逻辑阻挡， 则任务无法继续, 该次任务请求将被丢弃
     * 2. 此项IO阻塞和读线程无关, 必须保证读写互不影响， 否则可能导致因为 io blocked 而无法继续收包
     */
    posix__boolean_t write_io_blocked;
    
    /* 接收超时和发送超时 */
    struct timeval rcvtimeo;
    struct timeval sndtimeo;
    
    /* IP头的 tos 项
     * Differentiated Services Field: Dirrerentiated Services Codepoint/Explicit Congestion Not fication 指定TOS段
     *  */
    int iptos;
    
    union {
        /* TCP 独占属性 */
        struct {
            /* TCP 数据包应用层解析偏移 */
            int rx_parse_offset;
            
            /* 因为 TCP 应用层解包将占据 packet 字段， 因此需要一个字段用于 recv */
            char *rx_buffer;
    
            /* 大包解读(大于 0x11000 但是不足 50MB 的TCP数据包) */
            char* lbdata; /* large block data */
            int lboffset; /* 当前已经赋值的大包数据段偏移 */
            int lbsize; /* 含包头的大包总长度 */
    
             /* 下层解包模板 */
            tst_t template;
    
            /* getsockopt(TCP_INFO) for Linux, {Free,Net}BSD */
            struct tcp_info *ktcp; 

            /* MSS of tcp link */
            int mss;
        };
        
        /* UDP 独占属性 */
        struct {
            /* （目前只用于 UDP 的）对象标记：广播属性 */
            int flag;
            
            /* 适用于 IP 组播的 mreq 对象 */
            struct ip_mreq *mreq;
        };
    };
} ncb_t;

/* 布尔状态表达， 非0则IO阻止， 否则 IO 可行 */
#define ncb_if_wblocked(ncb)    (ncb->write_io_blocked)

/* 对这个 NCB 执行 IO 阻塞 */
#define ncb_mark_wblocked(ncb)   \
        do { if (!ncb->write_io_blocked) posix__atomic_xchange(&ncb->write_io_blocked, posix__true); } while (0);

/* 对这个 NCB 取消 IO 阻塞 */
#define ncb_cancel_wblock(ncb) posix__atomic_xchange(&ncb->write_io_blocked, posix__false);

#define ncb_lb_marked(ncb) ((ncb) ? ((NULL != ncb->lbdata) && (ncb->lbsize > 0)) : (posix__false))

extern
int ncb_init(ncb_t *ncb);
extern
void ncb_uninit(objhld_t ignore, void */*ncb_t * */ncb);
extern
void ncb_report_debug_information(ncb_t *ncb, const char *fmt, ...);

extern
int ncb_set_rcvtimeo(ncb_t *ncb, struct timeval *timeo);
extern
int ncb_get_rcvtimeo(ncb_t *ncb);
extern
int ncb_set_sndtimeo(ncb_t *ncb, struct timeval *timeo);
extern
int ncb_get_sndtimeo(ncb_t *ncb);

extern
int ncb_set_iptos(ncb_t *ncb, int tos);
extern
int ncb_get_iptos(ncb_t *ncb);

extern
int ncb_set_window_size(ncb_t *ncb, int dir, int size);
extern
int ncb_get_window_size(ncb_t *ncb, int dir, int *size);

extern
int ncb_set_linger(ncb_t *ncb, int onoff, int lin);
extern
int ncb_get_linger(ncb_t *ncb, int *onoff, int *lin);

extern
void ncb_post_preclose(ncb_t *ncb);
extern
void ncb_post_close(ncb_t *ncb);
extern
void ncb_post_recvdata(ncb_t *ncb,  int cb, const char *data);
extern
void ncb_post_accepted(ncb_t *ncb, HTCPLINK link);
extern
void ncb_post_senddata(ncb_t *ncb,  int cb, const char *data);
extern
void ncb_post_connected(ncb_t *ncb);

#endif