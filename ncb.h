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
#include <netinet/in.h>
#include <arpa/inet.h>

#include "posix_types.h"
#include "posix_thread.h"
#include "posix_atomic.h"

#include "nis.h"
#include "ncb.h"
#include "io.h"
#include "worker.h"
#include "fque.h"
#include "clist.h"

enum ncb__protocol_type_t {
    kProtocolType_Unknown = 0,
    kProtocolType_TCP,
    kProtocolType_UDP,
};

typedef struct _ncb {
    int hld;
    int sockfd;
    enum ncb__protocol_type_t proto_type;

    /* 普通收包，解包的参数字段 */
    char *packet;
    int rx_parse_offset;
    char *rx_buffer;
    
    /* Rx过程（TCP）需要保证包序，同时又需要epoll准确触发，一次建立的保证机制 
     * UDP 不受此约束*/
    posix__pthread_mutex_t rx_prot_lock;
    int rx_order_count;
    posix__boolean_t rx_running;
    
    
    /* 发送操作的顺序队列 */
    struct packet_fifo_t tx_fifo;

    /* 地址结构信息 */
    struct sockaddr_in remot_addr;
    struct sockaddr_in local_addr;

    /* 用户上下文和回调例程 */
    nis_callback_t nis_callback;
    char *context;
    int context_size;
    
    /* 下层解包模板 */
    tst_t template;

    /* 大包解读(大于 0x11000 但是不足 50MB 的TCP数据包) */
    char* lbdata; /* large block data */
    int lboffset; /* 当前已经赋值的大包数据段偏移 */
    int lbsize; /* 含包头的大包总长度 */

    /* （目前只用于 UDP 的）对象标记：广播属性 */
    int flag;

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
void ncb_uninit(int ignore, void */*ncb_t * */ncb);
extern
void ncb_report_debug_information(ncb_t *ncb, const char *fmt, ...);

#endif