#if !defined UDP_H_20170121
#define UDP_H_20170121

#include "ncb.h"

#if !defined UDP_BUFFER_SIZE
#define UDP_BUFFER_SIZE          	(0xFFFF)
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

extern
int udp_setattr_r(ncb_t *ncb, int attr);
extern
int udp_getattr_r(ncb_t *ncb, int *attr);

#endif
