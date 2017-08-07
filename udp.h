#if !defined UDP_H_20170121
#define UDP_H_20170121

#include "ncb.h"

#define UDP_MAXIMUM_SENDER_CACHED_CNT	( 44 ) /* 以每个包 1460 计, 最多可以接受 64KB 的发送堆积 */

#if !defined SO_MAX_MSG_SIZE
#define SO_MAX_MSG_SIZE   0x2003      /* maximum message size */
#endif

#if !defined UDP_BUFFER_SIZE
#define UDP_BUFFER_SIZE          (0xFFFF) 
#endif

/* udp io */
extern
int udp_rx(ncb_t *ncb);
extern
int udp_tx(ncb_t *ncb);

extern
int udp_set_boardcast(ncb_t *ncb, int enable);
extern
int udp_get_boardcast(ncb_t *ncb, int *enabled);


#endif