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

#define RX_NODE_BUFFER_SIZE (0x11000)

enum ncb__protocol_type_t {
    kProtocolType_Unknown = 0,
    kProtocolType_TCP,
    kProtocolType_UDP,
};

typedef struct {
    char                buffer_[RX_NODE_BUFFER_SIZE];
    uint32_t            offset_;    
    struct list_head    link_;
}rx_node_t;

typedef struct _ncb {
    int hld_;
    int fd_;
    enum ncb__protocol_type_t proto_type_;

    /* ��ͨ�հ�������Ĳ����ֶ� */
    char *packet_;
    int packet_size_;
    int recv_analyze_offset_;
    char *recv_buffer_;
    struct list_head rx_list_;
    posix__pthread_mutex_t rx_lock_;
    posix__boolean_t rx_parsing_;
    
    /* ���Ͳ�����˳����� */
    packet_fifo_t packet_fifo_;

    /* ��ַ�ṹ��Ϣ */
    struct sockaddr_in addr_remote_;
    struct sockaddr_in addr_local_;

    /* �û��ص����� */
    nis_callback_t user_callback_;

    /* �²���ģ�� */
    tst_t tst_;

    /* ������(���� 0x11000 ���ǲ��� 50MB ��TCP���ݰ�) */
    char* lb_data_; /* large block data */
    int lb_cpy_offset_; /* ��ǰ�Ѿ���ֵ�Ĵ�����ݶ�ƫ�� */
    int lb_length_; /* ����ͷ�Ĵ���ܳ��� */

    /* �û������� */
    char *user_context_;
    int user_context_size_;

    /* ��Ŀǰֻ���� UDP �ģ������ǣ��㲥���� */
    int flag_;

    /* IO ��Ӧ���� */
    int (*on_read_)(struct _ncb *);
    int (*on_write_)(struct _ncb *);
    int (*on_parse_)(struct _ncb *);
    
    /* ��Ҫ:
     * ���� write �������� EAGAIN ��
     * һ������ EAGAIN, ��������ֻ��ͨ�� EPOLLOUT ���������崦���ʩΪ
     * 1. ���� EAGAIN, �򼤻�� FD �� EPOLLOUT�¼���ͬʱ�� shield_ �� 1
     * 2. �� shield_ > 0 ������£��κ�д���� �������������� write
     * 3. EPOLLOUT �¼�����ʱ���� shield_ ��0�� ͬʱȡ�� EPOLLOUT �Ĺ�ע
     * ע��:
     * 1. һ�������߼��赲�� �������޷�����, �ô��������󽫱�����
     * 2. ����IO�����Ͷ��߳��޹�, ���뱣֤��д����Ӱ�죬 ������ܵ�����Ϊ io blocked ���޷������հ�
     */
    posix__boolean_t write_io_blocked_;
} ncb_t;

/* ����״̬�� ��0��IO��ֹ�� ���� IO ���� */
#define ncb_if_wblocked(ncb)    (ncb->write_io_blocked_)

/* ����� NCB ִ�� IO ���� */
#define ncb_mark_wblocked(ncb)   \
        do { if (!ncb->write_io_blocked_) posix__atomic_xchange(&ncb->write_io_blocked_, posix__true); } while (0);

/* ����� NCB ȡ�� IO ���� */
#define ncb_cancel_wblock(ncb) posix__atomic_xchange(&ncb->write_io_blocked_, posix__false);

#define ncb_lb_marked(ncb) ((ncb) ? ((NULL != ncb->lb_data_) && (ncb->lb_length_ > 0)) : (posix__false))

extern
int ncb_init(ncb_t *ncb);
extern
void ncb_uninit(int ignore, void */*ncb_t * */ncb);
extern
void ncb_report_debug_information(ncb_t *ncb, const char *fmt, ...);

#endif