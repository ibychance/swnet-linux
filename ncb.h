#if !defined NETCONTROLBLOCK
#define NETCONTROLBLOCK

#include "compiler.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/epoll.h>

#include <netinet/tcp.h>
#include <netinet/in.h>
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
    int epfd;  /* 绑定的EPOLL描述符 */
    enum ncb__protocol_type proto_type;

    /* 应用层数据包的收包实际缓冲区 */
    char *packet;
    
    /* 发送操作的顺序队列 */
    struct tx_fifo fifo;

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

            /* MSS of tcp link */
            int mss;
        } tcp;
        
        /* UDP 独占属性 */
        struct {
            /* （目前只用于 UDP 的）对象标记：广播属性 */
            int flag;
            
            /* 适用于 IP 组播的 mreq 对象 */
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