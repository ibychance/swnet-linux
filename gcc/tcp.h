#if !defined TCP_H_20170118
#define TCP_H_20170118

#include "ncb.h"

#if !defined TCP_BUFFER_SIZE
#define TCP_BUFFER_SIZE   ( 0x11000 )
#endif

#if !defined TCP_MAXIMUM_PACKET_SIZE
#define TCP_MAXIMUM_PACKET_SIZE  ( 50 << 20 )
#endif

#define TCP_MAXIMUM_SENDER_CACHED_CNT ( 5120 ) /* 以每个包64KB计, 最多可以接受 327MB 的发送堆积 */

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

#endif