#if !defined ARP_H_20190625
#define ARP_H_20190625

#include "ncb.h"

#pragma pack(push, 1)

struct Ethernet_Head
{
	unsigned char Eth_Dest_Mac[6];
	unsigned char Eth_Srce_Mac[6];
	unsigned short Eth_Layer_Type;
};

struct Address_Resolution_Protocol
{
	unsigned short Arp_Hardware_Type;
	unsigned short Arp_Protocol_Type;
	unsigned char Arp_Hardware_Size;
	unsigned char Arp_Protocol_Size;
	unsigned short Arp_Op_Code;
	unsigned char Arp_Sender_Mac[6];
	unsigned int Arp_Sender_Ip;
	unsigned char Arp_Target_Mac[6];
	unsigned int Arp_Target_Ip;
};

union arp_layer
{
	struct Address_Resolution_Protocol arp;
	unsigned char Eth_Padding[46];
};

#pragma pack(pop)

#define ARP_OP_REQ		(1)
#define ARP_OP_REPLY	(2)

#define NIS_P_ARP_SIZE (sizeof(struct Ethernet_Head ) + sizeof(union arp_layer))

/* arp io */
extern
int arp_rx(ncb_t *ncb);
extern
int arp_txn(ncb_t *ncb, void *p);
extern
int arp_tx(ncb_t *ncb);

#endif /* ARP_H_20190625 */
