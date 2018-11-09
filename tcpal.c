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

    /* 没有指定包头模板， 直接回调整个TCP包 */
    if (0 == ncb->u.tcp.template.cb_ || !ncb->u.tcp.template.parser_) {
        ncb_post_recvdata(ncb, cpcb, data);
        ncb->u.tcp.rx_parse_offset = 0;
        return 0;
    }

    /*已经被标记为大包接收状态 ，则处理大包数据*/
    if (ncb_lb_marked(ncb)) {
        /*本次到达数据尚不足以填满大包*/
        if (cpcb + ncb->u.tcp.lboffset < ncb->u.tcp.lbsize) {
            memcpy(ncb->u.tcp.lbdata + ncb->u.tcp.lboffset, cpbuff, cpcb);
            ncb->u.tcp.lboffset += cpcb;
            return 0;
        }

        /*本次到达数据足以完成大包*/
        overplus = ncb->u.tcp.lbsize - ncb->u.tcp.lboffset;
        memcpy(ncb->u.tcp.lbdata + ncb->u.tcp.lboffset, cpbuff, overplus);

        /*完成组包, 回调给上层模块*/
        ncb_post_recvdata(ncb, ncb->u.tcp.lbsize - ncb->u.tcp.template.cb_, ncb->u.tcp.lbdata + ncb->u.tcp.template.cb_);

        /*释放大包缓冲区*/
        free(ncb->u.tcp.lbdata);
        ncb->u.tcp.lbdata = NULL;
        ncb->u.tcp.lboffset = 0;
        ncb->u.tcp.lbsize = 0;
        return (cpcb - overplus);
    }

    /*数据长度尚不足构成协议头, 全部数据用于构建协议头， 返回剩余长度0*/
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

    /* 底层协议交互给协议模板处理， 处理失败则解包操作无法继续 */
    if (!(*ncb->u.tcp.template.parser_)) {
        nis_call_ecr("nshost.tcpal.parse : parser tempalte method illegal.");
        return -1;
    }

	/* 通过解释例程得到用户段数据长度 */
    if ((*ncb->u.tcp.template.parser_)(ncb->packet, ncb->u.tcp.rx_parse_offset, &user_data_size) < 0) {
        nis_call_ecr("nshost.tcpal.parse : failed to parse template header.");
		return -1;
	}
	
	/* 如果用户数据长度超出最大容忍长度，则直接报告为错误, 有可能是恶意攻击 */
    if ((user_data_size > TCP_MAXIMUM_PACKET_SIZE) || (user_data_size <= 0)) {
        nis_call_ecr("nshost.tcpal.parse : bad data size:%d.", user_data_size);
		return -1;
	}
	
    /* 含包头的包总长度 */
    total_packet_length = user_data_size + ncb->u.tcp.template.cb_;

    /* 如果是大包，则应该建立大包流程 */
    if (total_packet_length > TCP_BUFFER_SIZE) {
        ncb->u.tcp.lbdata = (char *) malloc(total_packet_length);
        if (!ncb->u.tcp.lbdata) {
            return -1;
        }

        /* 含底层协议包头的大包数据总长度 */
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
