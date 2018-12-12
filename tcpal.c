#include <stdio.h>

#include "tcp.h"
#include "mxx.h"

int tcp_parse_pkt(ncb_t *ncb, const char *data, int cpcb) {
    int used;
    int overplus;
    const char *cpbuff;
    int total_packet_length;
    int user_data_size;
    int retcb;

    if (!ncb || !data || 0 == cpcb) return -1;

    cpbuff = data;

    /* no template specified, direct give the whole packet */
    if (0 == ncb->u.tcp.template.cb_ || !ncb->u.tcp.template.parser_) {
        ncb_post_recvdata(ncb, cpcb, data);
        ncb->u.tcp.rx_parse_offset = 0;
        return 0;
    }

    /* it is in the large-block status */
    if (ncb_lb_marked(ncb)) {
        /* The arrival data are not enough to fill the large-block. */
        if (cpcb + ncb->u.tcp.lboffset < ncb->u.tcp.lbsize) {
            memcpy(ncb->u.tcp.lbdata + ncb->u.tcp.lboffset, cpbuff, cpcb);
            ncb->u.tcp.lboffset += cpcb;
            return 0;
        }

        /* The arrival data are it's enough to fill the large-block. */
        overplus = ncb->u.tcp.lbsize - ncb->u.tcp.lboffset;
        memcpy(ncb->u.tcp.lbdata + ncb->u.tcp.lboffset, cpbuff, overplus);

        ncb_post_recvdata(ncb, ncb->u.tcp.lbsize - ncb->u.tcp.template.cb_, ncb->u.tcp.lbdata + ncb->u.tcp.template.cb_);

        /* fre the large-block buffer */
        free(ncb->u.tcp.lbdata);
        ncb->u.tcp.lbdata = NULL;
        ncb->u.tcp.lboffset = 0;
        ncb->u.tcp.lbsize = 0;
        return (cpcb - overplus);
    }

    /* the length of data is not enough to constitute the protocol header. 
    *  All data is used to construct the protocol header and return the remaining length of 0. */
    if (ncb->u.tcp.rx_parse_offset + cpcb < ncb->u.tcp.template.cb_) {
        memcpy(ncb->packet + ncb->u.tcp.rx_parse_offset, cpbuff, cpcb);
        ncb->u.tcp.rx_parse_offset += cpcb;
        return 0;
    }

    overplus = cpcb;
    used = 0;

    /*当前包中的数据不足以构建协议头，但是加上本次数据，足以构造协议头*/
    if (ncb->u.tcp.rx_parse_offset < ncb->u.tcp.template.cb_) {
        used += (ncb->u.tcp.template.cb_ - ncb->u.tcp.rx_parse_offset);
        overplus = cpcb - used;
        memcpy(ncb->packet + ncb->u.tcp.rx_parse_offset, cpbuff, used);
        cpbuff += used;
        ncb->u.tcp.rx_parse_offset = ncb->u.tcp.template.cb_;
    }

    /* The low-level protocol interacts with the protocol template, and the unpacking operation cannot continue if the processing fails.  */
    if (!(*ncb->u.tcp.template.parser_)) {
        nis_call_ecr("nshost.tcpal.parse : parser tempalte method illegal.");
        return -1;
    }

	/* Get the length of user segment data by interpreting routines  */
    if ((*ncb->u.tcp.template.parser_)(ncb->packet, ncb->u.tcp.rx_parse_offset, &user_data_size) < 0) {
        nis_call_ecr("nshost.tcpal.parse : failed to parse template header.");
		return -1;
	}
	
	/* If the user data length exceeds the maximum tolerance length, 
     * it will be reported as an error directly, possibly a malicious attack.  */
    if ((user_data_size > TCP_MAXIMUM_PACKET_SIZE) || (user_data_size <= 0)) {
        nis_call_ecr("nshost.tcpal.parse : bad data size:%d.", user_data_size);
		return -1;
	}
	
    /* total package length, include the packet head */
    total_packet_length = user_data_size + ncb->u.tcp.template.cb_;

    /* If it is a large-block, then we should establish a large-block process.  */
    if (total_packet_length > TCP_BUFFER_SIZE) {
        ncb->u.tcp.lbdata = (char *) malloc(total_packet_length);
        if (!ncb->u.tcp.lbdata) {
            return -1;
        }

        /* total large-block length, include the low-level protocol head length */
        ncb->u.tcp.lbsize = total_packet_length;

        /* 本次全部数据拷贝到大包缓冲区 */
        memcpy(ncb->u.tcp.lbdata, data, cpcb);
        ncb->u.tcp.lboffset = cpcb;

        /* 清理普通包缓冲区的描述信息 */
        ncb->u.tcp.rx_parse_offset = 0;

        /* 大包构建阶段，单次接收缓冲区的数据肯定会一次用尽 */
        return 0;
    }

    /*剩余的字节数，足以构造整个包*/
    if ((ncb->u.tcp.rx_parse_offset + overplus) >= total_packet_length) {
        memcpy(ncb->packet + ncb->u.tcp.rx_parse_offset, cpbuff, total_packet_length - ncb->u.tcp.rx_parse_offset);

        /*返回给上级调用的字节数=本次的剩余字节数-构建本包总共消耗的字节数*/
        retcb = (overplus - (total_packet_length - ncb->u.tcp.rx_parse_offset));

        /*完成组包, 回调给上层模块*/
        ncb_post_recvdata(ncb, user_data_size, ncb->packet + ncb->u.tcp.template.cb_);

        ncb->u.tcp.rx_parse_offset = 0;
        return retcb;
    }

    /*剩余字节数不足以构造一个完整包， 则把剩余字节数都放入缓冲区，并调整包解析偏移*/
    memmove(ncb->packet + ncb->u.tcp.rx_parse_offset, cpbuff, overplus);
    ncb->u.tcp.rx_parse_offset += overplus;
    return 0;
}
