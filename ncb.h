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
    int epfd;  /* �󶨵�EPOLL������ */
    enum ncb__protocol_type_t proto_type;

    /* ��ͨ�հ�������Ĳ����ֶ� */
    char *packet;
    int rx_parse_offset;
    char *rx_buffer;
    
    /* ���Ͳ�����˳����� */
    struct packet_fifo_t tx_fifo;

    /* ��ַ�ṹ��Ϣ */
    struct sockaddr_in remot_addr;
    struct sockaddr_in local_addr;

    /* �û������ĺͻص����� */
    nis_callback_t nis_callback;
    char *context;
    int context_size;
    
    /* �²���ģ�� */
    tst_t template;

    /* ������(���� 0x11000 ���ǲ��� 50MB ��TCP���ݰ�) */
    char* lbdata; /* large block data */
    int lboffset; /* ��ǰ�Ѿ���ֵ�Ĵ�����ݶ�ƫ�� */
    int lbsize; /* ����ͷ�Ĵ���ܳ��� */

    /* ��Ŀǰֻ���� UDP �ģ������ǣ��㲥���� */
    int flag;

    /* IO ��Ӧ���� */
    int (*ncb_read)(struct _ncb *);
    int (*ncb_write)(struct _ncb *);
    
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
    posix__boolean_t write_io_blocked;
    
    /* ���ճ�ʱ�ͷ��ͳ�ʱ */
    struct timeval rcvtimeo;
    struct timeval sndtimeo;
    
    /* IPͷ�� tos ��
     * Differentiated Services Field: Dirrerentiated Services Codepoint/Explicit Congestion Not fication ָ��TOS��
     *  */
    int iptos;
    
    /* getsockopt(TCP_INFO) for Linux, {Free,Net}BSD */
    struct tcp_info *ktcp; 
    
    /* MSS of tcp link */
    int mss;
    
} ncb_t;

/* ����״̬�� ��0��IO��ֹ�� ���� IO ���� */
#define ncb_if_wblocked(ncb)    (ncb->write_io_blocked)

/* ����� NCB ִ�� IO ���� */
#define ncb_mark_wblocked(ncb)   \
        do { if (!ncb->write_io_blocked) posix__atomic_xchange(&ncb->write_io_blocked, posix__true); } while (0);

/* ����� NCB ȡ�� IO ���� */
#define ncb_cancel_wblock(ncb) posix__atomic_xchange(&ncb->write_io_blocked, posix__false);

#define ncb_lb_marked(ncb) ((ncb) ? ((NULL != ncb->lbdata) && (ncb->lbsize > 0)) : (posix__false))

extern
int ncb_init(ncb_t *ncb);
extern
void ncb_uninit(int ignore, void */*ncb_t * */ncb);
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
int ncb_set_keepalive(ncb_t *ncb, int enable);
extern
int ncb_get_keepalive(ncb_t *ncb, int *enabled);

#endif