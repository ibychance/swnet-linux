#include <stdio.h>

#include "udp.h"

#if !defined MAX_UDP_SIZE
#define MAX_UDP_SIZE		(MTU - (ETHERNET_P_SIZE + IP_P_SIZE + UDP_P_SIZE))
#endif

extern
void (*__notify_nshost_dbglog)(const char *logstr);

static int udp_update_opts(ncb_t *ncb) {
    static const int RECV_BUFFER_SIZE = 0xFFFF;
    static const int SEND_BUFFER_SIZE = 0xFFFF;

    if (!ncb) {
        return -1;
    }

    ncb_set_window_size(ncb, SO_RCVBUF, RECV_BUFFER_SIZE);
    ncb_set_window_size(ncb, SO_SNDBUF, SEND_BUFFER_SIZE);
    ncb_set_linger(ncb, 0, 1);
    return 0;
}

int udp_init() {
    if (ioinit() >= 0) {
        write_pool_init();
    }
    return -1;
}

void udp_uninit() {
    iouninit();
    write_pool_uninit();
}

HUDPLINK udp_create(udp_io_callback_t user_callback, const char* l_ipstr, uint16_t l_port, int flag) {
    int fd;
    struct sockaddr_in addrlocal;
    int retval;
    int optval;
    objhld_t hld;
    socklen_t addrlen;
    ncb_t *ncb;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("failed create services socket.errno=%d.\n", errno);
        return -1;
    }

    optval = 1;
    retval = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof ( optval));

    addrlocal.sin_addr.s_addr = l_ipstr ? inet_addr(l_ipstr) : INADDR_ANY;
    addrlocal.sin_family = AF_INET;
    addrlocal.sin_port = htons(l_port);
    retval = bind(fd, (struct sockaddr *) &addrlocal, sizeof ( struct sockaddr));
    if (retval < 0) {
        close(fd);
        return -1;
    }

    hld = objallo(sizeof ( ncb_t), NULL, &ncb_uninit, NULL, 0);
    if (hld < 0) {
        close(fd);
        return -1;
    }
    ncb = (ncb_t *) objrefr(hld);

    do {
        /* ������ʼ�� */
        ncb_init(ncb);

        /* ���Ʋ��� */
        ncb->nis_callback = user_callback;
        ncb->sockfd = fd;
        ncb->hld = hld;
        ncb->proto_type = kProtocolType_UDP;

        /*ETģ�ͱ��뱣�������ļ��������첽����*/
        if (setasio(fd) < 0) {
            break;
        }

        /* setsockopt */
        if (udp_update_opts(ncb) < 0) {
            break;
        }

        /*�����������*/
        ncb->packet = (char *) malloc(UDP_BUFFER_SIZE);
        if (!ncb->packet) {
            break;
        }

        /* ����㲥/�鲥���� */
        if (flag & UDP_FLAG_BROADCAST) {
            if (udp_set_boardcast(ncb, 1) < 0) {
                break;
            }
            ncb->flag |= UDP_FLAG_BROADCAST;
        } else {
            if (flag & UDP_FLAG_MULTICAST) {
                ncb->flag |= UDP_FLAG_MULTICAST;
            }
        }

        /* ��ȡ���ص�ַ */
        getsockname(ncb->sockfd, (struct sockaddr *) &ncb->local_addr, &addrlen);

        /* ��ע���ݰ� */
        ncb->ncb_read = &udp_rx;
        ncb->ncb_write = &udp_tx;
        
        /* ���ӵ� EPOLL */
        retval = ioatth(ncb, EPOLLIN);
        if (retval < 0) {
            break;
        }
        
        return hld;
    } while (0);

    objdefr(hld);
    objclos(hld);
    return -1;
}

void udp_destroy(HUDPLINK lnk) {
    objclos(lnk);
}

static int udp_maker(void *data, int cb, void *context) {
    if (data && cb > 0 && context) {
        memcpy(data, context, cb);
        return 0;
    }
    return -1;
}

int udp_write(HUDPLINK lnk, int cb, nis_sender_maker_t maker, void *par, const char* r_ipstr, uint16_t r_port) {
    int retval;
    ncb_t *ncb;
    struct sockaddr_in addr;
    objhld_t hld = (objhld_t) lnk;
    unsigned char *buffer;

    if ( !r_ipstr || (0 == r_port) || (cb <= 0) || (lnk < 0) || (cb > UDP_MAXIMUM_USER_DATA_SIZE)) {
        return -EINVAL;
    }

    ncb = (ncb_t *) objrefr(hld);
    if (!ncb) {
        return -ENOENT;
    }

    retval = -1;
    buffer = NULL;
    do {
        /* ���Ͷ��й����� �޷����з��Ͳ��� */
        if ((fque_size(&ncb->tx_fifo) >= UDP_MAXIMUM_SENDER_CACHED_CNT)) {
            break;
        }

        buffer = (unsigned char *) malloc(cb);
        if (!buffer) {
            retval = -ENOMEM;
            break;
        }

        /* �Լ���������� */
        if (maker) {
            if ((*maker)(buffer, cb, par) < 0) {
                break;
            }
        }else{
            if (udp_maker(buffer, cb, par) < 0){
                break;
            }
        }
        

        /* ���Ͷ�������һ���ڵ�, ��Ͷ�ݼ�����Ϣ */
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(r_ipstr);
        addr.sin_port = htons(r_port);

        /* ���Ͷ�������һ���ڵ�, ��Ͷ�ݼ�����Ϣ */
        if (fque_push(&ncb->tx_fifo, buffer, cb, &addr) < 0) {
            break;
        }
        post_write_task(hld, kTaskType_TxTest);
        
        objdefr(hld);
        return 0;
    } while (0);

    if (buffer) {
        free(buffer);
    }
    objdefr(hld);
    return retval;
}

int udp_getaddr(HUDPLINK lnk, uint32_t *ipv4, uint16_t *port) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;

    ncb = objrefr(hld);
    if (!ncb) return -1;

    *ipv4 = htonl(ncb->local_addr.sin_addr.s_addr);
    *port = htons(ncb->local_addr.sin_port);

    objdefr(hld);
    return 0;
}

int udp_setopt(HUDPLINK lnk, int level, int opt, const char *val, int len) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int retval;

    ncb = objrefr(hld);
    if (!ncb) return -1;

    retval = setsockopt(ncb->sockfd, level, opt, val, len);

    objdefr(hld);
    return retval;
}

int udp_getopt(HUDPLINK lnk, int level, int opt, char *val, int *len) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int retval;

    ncb = objrefr(hld);
    if (!ncb) return -1;

    retval = getsockopt(ncb->sockfd, level, opt, val, (socklen_t *)len);

    objdefr(hld);
    return retval;
}

int udp_set_boardcast(ncb_t *ncb, int enable) {
    if (ncb) {
        return setsockopt(ncb->sockfd, SOL_SOCKET, SO_BROADCAST, (const void *) &enable, sizeof (enable));
    }
    return -EINVAL;
}

int udp_get_boardcast(ncb_t *ncb, int *enabled) {
    if (ncb && enabled) {
        socklen_t optlen = sizeof (int);
        return getsockopt(ncb->sockfd, SOL_SOCKET, SO_BROADCAST, (void * __restrict)enabled, &optlen);
    }
    return -EINVAL;
}

/*
 * �鲥���ĵ�Ŀ�ĵ�ַʹ��D��IP��ַ�� D���ַ���ܳ�����IP���ĵ�ԴIP��ַ�ֶΡ��������ݴ�������У�һ�����ݰ������·���Ǵ�Դ��ַ·�ɵ�Ŀ�ĵ�ַ�����á���������ԭ��[·��ѡ��]��IP�����д��䡣
 * Ȼ����ip�鲥���У����ݰ���Ŀ�ĵ�ַ����һ��������һ�飬�γ����ַ�����е���Ϣ�����߶����뵽һ�����ڣ�����һ������֮���������ַ������������ʼ������ߴ��䣬���е����г�Ա���ܽ��յ����ݰ���
 * �鲥���еĳ�Ա�Ƕ�̬�ģ������������κ�ʱ�̼�����뿪�鲥�顣
 *         ��ͬһ��IP�ಥ��ַ���նಥ���ݰ�����������������һ�������飬Ҳ��Ϊ�ಥ�顣һ���ಥ��ĳ�Ա����ʱ�䶯�ģ�һ̨����������ʱ������뿪�ಥ�飬�ಥ���Ա����Ŀ�����ڵĵ���λ��Ҳ�������ƣ�һ̨����Ҳ�������ڼ����ಥ�顣
 * ���⣬������ĳһ���ಥ�������Ҳ������öಥ�鷢�����ݰ���  
 * 
 * �鲥��ַ
 * �鲥����������õ�Ҳ��������ʱ�ġ��鲥���ַ�У���һ�����ɹٷ�����ģ���Ϊ�����鲥�顣
 * �����鲥�鱣�ֲ����������ip��ַ�����еĳ�Ա���ɿ��Է����仯�������鲥���г�Ա������������������ģ���������Ϊ�㡣��Щû�б��������������鲥��ʹ�õ�ip�鲥��ַ�����Ա���ʱ�鲥�����á�
 *      224.0.0.0��224.0.0.255ΪԤ�����鲥��ַ���������ַ������ַ224.0.0.0�����������䣬������ַ��·��Э��ʹ��
 *      224.0.1.0��224.0.1.255�ǹ����鲥��ַ����������Internet
 *      224.0.2.0��238.255.255.255Ϊ�û����õ��鲥��ַ����ʱ���ַ����ȫ����Χ����Ч
 *      239.0.0.0��239.255.255.255Ϊ���ع����鲥��ַ�������ض��ı��ط�Χ����Ч
 * 
 *  �鲥��һ�Զ�Ĵ��䷽ʽ�������и��鲥��� ������Ͷ˽�������һ�����ڷ��ͣ������е�·����ͨ���ײ��IGMPЭ���Զ������ݷ��͵����м����������նˡ�
 *  ���ڹ㲥����鲥��һЩ���ƣ�������·�����������ڵ�ÿһ���ն˶�Ͷ��һ�����ݰ���������Щ�ն��Ƿ����ڽ��ո����ݰ���UDP�㲥ֻ����������ͬһ���Σ���Ч�����鲥���ԽϺ�ʵ�ֿ�����Ⱥ�����ݡ�
 *   UDP�鲥�ǲ��õ�������,���ݱ������ӷ�ʽ�������ǲ��ɿ��ġ�Ҳ���������ܲ��ܵ�����ܶ˺����ݵ����˳���ǲ��ܱ�֤�ġ���������UDP���ñ�֤���� �Ŀɿ��ԣ��������ݵĴ���Ч���Ǻܿ�ġ�
 */
int udp_joingrp(HUDPLINK lnk, const char *g_ipstr, uint16_t g_port) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int retval;

    if (lnk < 0 || !g_ipstr || 0 == g_port) {
        return -EINVAL;
    }

    ncb = objrefr(hld);
    if (!ncb) return -1;

    do {
        retval = -1;

        if (!(ncb->flag & UDP_FLAG_MULTICAST)) {
            break;
        }

        /*���ûػ����*/
        int loop = 1;
        retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const void *)&loop, sizeof (loop));
        if (retval < 0) {
            break;
        }
        
        /*����ಥ��*/
        if (!ncb->mreq){
            ncb->mreq = (struct ip_mreq *)malloc(sizeof(struct ip_mreq));
        }
        ncb->mreq->imr_multiaddr.s_addr = inet_addr(g_ipstr); 
        ncb->mreq->imr_interface.s_addr = ncb->local_addr.sin_addr.s_addr; 
        retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const void *)ncb->mreq, sizeof(struct ip_mreq));
        if (retval < 0){
            break;
        }
        
    } while (0);

    objdefr(hld);
    return retval;
}

int udp_dropgrp(HUDPLINK lnk){
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int retval;
    
    if (lnk < 0){
        return -EINVAL;
    }
    
    ncb = objrefr(hld);
    if (!ncb) return -1;
    
    do{
        retval = -1;
        
        if (!(ncb->flag & UDP_FLAG_MULTICAST) || !ncb->mreq) {
            break;
        }
        
        /*��ԭ�ػ����*/
        int loop = 0;
        retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, (const void *)&loop, sizeof (loop));
        if (retval < 0) {
            break;
        }
        
        /*�뿪�ಥ��*/
        retval = setsockopt(ncb->sockfd, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const void *)ncb->mreq, sizeof(struct ip_mreq));
        
    }while(0);
    
    objdefr(hld);
    return retval;
}