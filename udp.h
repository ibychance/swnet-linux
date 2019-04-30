#if !defined UDP_H_20170121
#define UDP_H_20170121

#include "ncb.h"

#if !defined SO_MAX_MSG_SIZE
#define SO_MAX_MSG_SIZE   			(0x2003)      /* maximum message size */
#endif

#if !defined UDP_BUFFER_SIZE
#define UDP_BUFFER_SIZE          	(0xFFFF)
#endif

#if !defined MAX_UDP_SIZE
#define MAX_UDP_SIZE				(MTU - IP_UDP_HEAD_SIZE)
#endif

/* udp io */
extern
int udp_rx(ncb_t *ncb);
extern
int udp_txn(ncb_t *ncb, void *p);
extern
int udp_tx(ncb_t *ncb);
extern
int udp_set_boardcast(ncb_t *ncb, int enable);
extern
int udp_get_boardcast(ncb_t *ncb, int *enabled);


#endif
