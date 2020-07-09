#include "arp.h"

#include <netdb.h>
#include <ifaddrs.h>

#include "fifo.h"
#include "io.h"
#include "wpool.h"
#include "mxx.h"

#include "posix_atomic.h"

#undef __USE_MISC
#include <net/if.h>

static
int __arprefr( objhld_t hld, ncb_t **ncb )
{
    if ( hld < 0 || !ncb) {
        return -EINVAL;
    }

    *ncb = objrefr( hld );
    if ( NULL != (*ncb) ) {
        if ( (*ncb)->protocol == ETH_P_ARP ) {
            return 0;
        }

        objdefr( hld );
        *ncb = NULL;
        return -EPROTOTYPE;
    }

    return -ENOENT;
}

static int arp_bindsource(const char *source, ncb_t *ncb)
{
    struct ifaddrs *ifa, *ifs, *ifsrc;
    uint32_t src_ip;

    src_ip = inet_addr(source);

    ifsrc = NULL;

    if (getifaddrs(&ifs) < 0) {
        return posix__makeerror(errno);
    }

    for (ifa = ifs; ifa != NULL; ifa = ifa->ifa_next) {
        if (((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr == src_ip) {
            ifsrc = ifa;
            break;
        }
    }

    if (!ifsrc) {
        return -ENOENT;
    }

    ncb->u.arp.source = src_ip;
    ncb->u.arp.ifindex = if_nametoindex(ifsrc->ifa_name);
    nis_getifmac(ifsrc->ifa_name, ncb->u.arp.source_phyaddr);
    return 0;
}

HARPLINK arp_create(arp_io_callback_t callback, const char *source)
{
    int fd;
    HARPLINK hld;
    ncb_t *ncb;
    int retval;
    int hdrincl;

    fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ARP));
    if (fd < 0) {
        nis_call_ecr("[nshost.arp.create] fatal error occurred syscall socket(2), error:%d", errno);
        return -1;
    }

    hld = objallo(sizeof ( ncb_t), &ncb_allocator, &ncb_deconstruct, NULL, 0);
    if (hld < 0) {
        nis_call_ecr("[nshost.arp.create] insufficient resource for allocate inner object");
        close(fd);
        return -1;
    }
    ncb = (ncb_t *) objrefr(hld);
    assert(ncb);

    do {
        /* copy initialize parameters */
        ncb->nis_callback = callback;
        ncb->sockfd = fd;
        ncb->hld = hld;
        ncb->protocol = ETH_P_ARP;

        hdrincl = 1;
        setsockopt(fd, IPPROTO_IP, IP_HDRINCL, (const char *)&hdrincl, sizeof(hdrincl));

        if ((retval = arp_bindsource(source, ncb)) < 0) {
            break;
        }

        /* allocate buffer for normal packet */
        if (NULL == (ncb->packet = (unsigned char *) malloc(sizeof(struct Ethernet_Head ) + sizeof(union arp_layer)))) {
            retval = -ENOMEM;
            break;
        }

        /* set data handler function pointer for Rx/Tx */
        posix__atomic_set(&ncb->ncb_read, &arp_rx);
        posix__atomic_set(&ncb->ncb_write, &arp_tx);

        /* attach to epoll */
        retval = io_attach(ncb, EPOLLIN);
        if (retval < 0) {
            break;
        }

        objdefr(hld);
        return hld;
    } while (0);

    objdefr(hld);
    objclos(hld);
    return -1;
}

void arp_destroy(HARPLINK link)
{
    ncb_t *ncb;

    /* it should be the last reference operation of this object no matter how many ref-count now. */
    ncb = objreff(link);
    if (ncb) {
        nis_call_ecr("[nshost.arp.destroy] link:%lld order to destroy", ncb->hld);
        io_close(ncb);
        objdefr(link);
    }
}

int arp_nrequest(HARPLINK link, uint32_t target)
{
    static const unsigned char BOARDCAST_PHYADDR[6] = {0xff,0xff,0xff,0xff,0xff,0xff};  /* boardcast pyhsical address */

    struct tx_node *node;
    unsigned char *arp_request;
    struct Ethernet_Head  *eth;
    struct Address_Resolution_Protocol *arp;
    int retval;
    ncb_t *ncb;

    retval = __arprefr(link, &ncb);
    if (retval < 0) {
        return retval;
    }

    arp_request = NULL;
    node = NULL;

    do {
        if (NULL == (arp_request = (unsigned char *)malloc(NIS_P_ARP_SIZE))) {
            retval = -ENOMEM;
            break;
        }

        /* fill Ethernet layer */
        eth = (struct Ethernet_Head  *)arp_request;
        memcpy(eth->Eth_Dest_Mac, BOARDCAST_PHYADDR, sizeof(BOARDCAST_PHYADDR));
        memcpy(eth->Eth_Srce_Mac, ncb->u.arp.source_phyaddr, sizeof(ncb->u.arp.source_phyaddr));
        eth->Eth_Layer_Type = htons(ETH_P_ARP);

        /* fill arp layer */
        arp = (struct Address_Resolution_Protocol *)&arp_request[sizeof(struct Ethernet_Head )];
        arp->Arp_Hardware_Type = htons(1);
        arp->Arp_Protocol_Type = htons(ETH_P_IP);
        arp->Arp_Hardware_Size  = 6;
        arp->Arp_Protocol_Size = 4;
        arp->Arp_Op_Code = htons(ARP_OP_REQ);
        memcpy(arp->Arp_Sender_Mac, ncb->u.arp.source_phyaddr, sizeof(ncb->u.arp.source_phyaddr));
        memcpy(&arp->Arp_Sender_Ip, &ncb->u.arp.source, sizeof(ncb->u.arp.source));
        memcpy(arp->Arp_Target_Mac, BOARDCAST_PHYADDR, sizeof(BOARDCAST_PHYADDR));
        memcpy(&arp->Arp_Target_Ip, &target, sizeof(target));

        /* create tx node, and use this node to send */
        if (NULL == (node = (struct tx_node *) malloc(sizeof (struct tx_node)))) {
            retval = -ENOMEM;
            break;
        }
        memset(node, 0, sizeof(struct tx_node));
        node->data = arp_request;
        node->wcb = NIS_P_ARP_SIZE;
        node->offset = 0;
        node->arp_target.sll_family = PF_PACKET;
        node->arp_target.sll_ifindex = ncb->u.arp.ifindex;

        if (!fifo_is_blocking(ncb)) {
            retval = arp_txn(ncb, node);
            if (-EAGAIN != retval) {
                break;
            }
        }
        retval = fifo_queue(ncb, node);
        if (retval < 0) {
            break;
        }

        objdefr(link);
        return 0;
    } while(0);

    if (arp_request) {
        free(arp_request);
    }

    if (node) {
        free(node);
    }

    objdefr(link);
    return retval;
}

int arp_request(HARPLINK link, const char *target)
{
    return arp_nrequest(link, inet_addr(target));
}
