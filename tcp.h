#if !defined TCP_H_20170118
#define TCP_H_20170118

#include "ncb.h"

#if !defined TCP_BUFFER_SIZE
#define TCP_BUFFER_SIZE   ( 0x11000 )
#endif

#if !defined TCP_MAXIMUM_PACKET_SIZE
#define TCP_MAXIMUM_PACKET_SIZE  ( 50 << 20 )
#endif

#define TCP_MAXIMUM_SENDER_CACHED_CNT ( 5120 ) /* ��ÿ����64KB��, �����Խ��� 327MB �ķ��Ͷѻ� */

#define NS_TCP_NODELAY_UNSET  (0)
#define NS_TCP_NODELAY_SET  (1)

extern
int tcp_update_opts(ncb_t *ncb);

/* tcp io */
extern
int tcp_syn(ncb_t *ncb_server);
extern
int tcp_rx(ncb_t *ncb);
extern
int tcp_tx(ncb_t *ncb);

/* tcp al */
extern
int tcp_parse_pkt(ncb_t *ncb, const char *data, int cpcb);


//for TCP_INFO socket option 
//#define TCPI_OPT_TIMESTAMPS 1
//#define TCPI_OPT_SACK 2
//#define TCPI_OPT_WSCALE 4
//#define TCPI_OPT_ECN 8
//
//struct tcp_info {
//    __u8 tcpi_state; TCP״̬
//    __u8 tcpi_ca_state; TCPӵ��״̬
//    __u8 tcpi_retransmits;  ��ʱ�ش��Ĵ��� 
//    __u8 tcpi_probes;  ������ʱ���򱣻ʱ��������δȷ�ϵĶ���
//    __u8 tcpi_backoff;  �˱�ָ�� 
//    __u8 tcpi_options; ʱ���ѡ�SACKѡ���������ѡ�ECNѡ���Ƿ�����
//    __u8 tcpi_snd_wscale : 4, tcpi_rcv_wscale : 4;  ���͡����յĴ�����������
//
//    __u32 tcpi_rto; ��ʱʱ�䣬��λΪ΢��
//    __u32 tcpi_ato;  ��ʱȷ�ϵĹ�ֵ����λΪ΢��
//    __u32 tcpi_snd_mss;  ���˵�MSS 
//    __u32 tcpi_rcv_mss; �Զ˵�MSS 
//
//    __u32 tcpi_unacked;  δȷ�ϵ����ݶ���������current listen backlog 
//    __u32 tcpi_sacked; SACKed�����ݶ���������listen backlog set in listen()
//    __u32 tcpi_lost; ��ʧ��δ�ָ������ݶ��� 
//    __u32 tcpi_retrans; �ش���δȷ�ϵ����ݶ���
//    __u32 tcpi_fackets; FACKed�����ݶ���
//
//    Times. ��λΪ����
//    __u32 tcpi_last_data_sent;  ���һ�η������ݰ��ڶ��֮ǰ 
//    __u32 tcpi_last_ack_sent;   �����á�Not remembered, sorry. 
//    __u32 tcpi_last_data_recv;  ���һ�ν������ݰ��ڶ��֮ǰ 
//    __u32 tcpi_last_ack_recv; ���һ�ν���ACK���ڶ��֮ǰ 
//
//    Metrics.
//    __u32 tcpi_pmtu; ���һ�θ��µ�·��MTU 
//    __u32 tcpi_rcv_ssthresh;  current window clamp��rcv_wnd����ֵ 
//    __u32 tcpi_rtt;  ƽ����RTT����λΪ΢�� 
//    __u32 tcpi_rttvar; /�ķ�֮һmdev����λΪ΢��v 
//    __u32 tcpi_snd_ssthresh; ��������ֵ 
//    __u32 tcpi_snd_cwnd; ӵ������ 
//    __u32 tcpi_advmss; �����ܽ��ܵ�MSS���ޣ��ڽ�������ʱ����ͨ��Զ� 
//    __u32 tcpi_reordering; û�ж���ʱ������������������ݶ���
//
//    __u32 tcpi_rcv_rtt ��Ϊ���նˣ������RTTֵ����λΪ΢��
//    __u32 tcpi_rcv_space;  ��ǰ���ջ���Ĵ�С
//
//    __u32 tcpi_total_retrans; �����ӵ����ش�����
//};
 
extern
int tcp_save_info(ncb_t *ncb);
extern
int tcp_setmss(ncb_t *ncb, int mss);
extern
int tcp_getmss(ncb_t *ncb);
extern
int tcp_set_nodelay(ncb_t *ncb, int set);
extern
int tcp_get_nodelay(ncb_t *ncb, int *set);

#endif