#include <stdio.h>

#include "tcp.h"

int tcp_parse_pkt(ncb_t *ncb, const char *data, int cpcb) {
    int used;
    int overplus;
    const char *cpbuff;
    nis_event_t c_event;
    tcp_data_t c_data;
    int total_packet_length;
    int user_data_size;
    int retcb;

    if (!ncb || !data || 0 == cpcb) return -1;

    cpbuff = data;

    /* 没有指定包头模板， 直接回调整个TCP包 */
    if (0 == ncb->tst_.cb_) {
        c_event.Ln.Tcp.Link = (HTCPLINK) ncb->hld_;
        c_event.Event = EVT_RECEIVEDATA;
        c_data.e.Packet.Size = cpcb;
        c_data.e.Packet.Data = (const char *) ((char *) data);
        if (ncb->user_callback_) {
            ncb->user_callback_(&c_event, &c_data);
        }
        ncb->recv_analyze_offset_ = 0;
        return 0;
    }

    /*已经被标记为大包接收状态 ，则处理大包数据*/
    if (ncb_lb_marked(ncb)) {
        /*本次到达数据尚不足以填满大包*/
        if (cpcb + ncb->lb_cpy_offset_ < ncb->lb_length_) {
            memcpy(ncb->lb_data_ + ncb->lb_cpy_offset_, cpbuff, cpcb);
            ncb->lb_cpy_offset_ += cpcb;
            return 0;
        }

        /*本次到达数据足以完成大包*/
        overplus = ncb->lb_length_ - ncb->lb_cpy_offset_;
        memcpy(ncb->lb_data_ + ncb->lb_cpy_offset_, cpbuff, overplus);

        /*完成组包, 回调给上层模块*/
        if (ncb->user_callback_) {
            c_event.Ln.Tcp.Link = ncb->hld_;
            c_event.Event = EVT_RECEIVEDATA;
            c_data.e.Packet.Data = (ncb->lb_data_ + ncb->tst_.cb_);
            c_data.e.Packet.Size = ncb->lb_length_ - ncb->tst_.cb_;
            ncb->user_callback_(&c_event, &c_data);
        }

        /*释放大包缓冲区*/
        free(ncb->lb_data_);
        ncb->lb_data_ = NULL;
        ncb->lb_cpy_offset_ = 0;
        ncb->lb_length_ = 0;
        return (cpcb - overplus);
    }

    /*数据长度尚不足构成协议头, 全部数据用于构建协议头， 返回剩余长度0*/
    if (ncb->recv_analyze_offset_ + cpcb < ncb->tst_.cb_) {
        memcpy(ncb->packet_ + ncb->recv_analyze_offset_, cpbuff, cpcb);
        ncb->recv_analyze_offset_ += cpcb;
        return 0;
    }

    overplus = cpcb;
    used = 0;

    /*当前包中的数据不足以构建协议头，但是加上本次数据，足以构造协议头*/
    if (ncb->recv_analyze_offset_ < ncb->tst_.cb_) {
        used += (ncb->tst_.cb_ - ncb->recv_analyze_offset_);
        overplus = cpcb - used;
        memcpy(ncb->packet_ + ncb->recv_analyze_offset_, cpbuff, used);
        cpbuff += used;
        ncb->recv_analyze_offset_ = ncb->tst_.cb_;
    }

    /* 底层协议交互给协议模板处理， 处理失败则解包操作无法继续 */
    if (!(*ncb->tst_.parser_)) {
        ncb_report_debug_information(ncb, "[TCP]invalidated link object TST parser function address.\n");
        return -1;
    }

    if ((*ncb->tst_.parser_)(ncb->packet_, ncb->recv_analyze_offset_, &user_data_size) < 0) return -1;
    if (user_data_size > TCP_MAXIMUM_PACKET_SIZE) return -1;

    /* 含包头的包总长度 */
    total_packet_length = user_data_size + ncb->tst_.cb_;

    /* 如果是大包，则应该建立大包流程 */
    if (total_packet_length > TCP_BUFFER_SIZE) {
        ncb->lb_data_ = (char *) malloc(total_packet_length);
        if (!ncb->lb_data_) {
            return -1;
        }

        /* 含底层协议包头的大包数据总长度 */
        ncb->lb_length_ = total_packet_length;

        /* 本次全部数据拷贝到大包缓冲区 */
        memcpy(ncb->lb_data_, data, cpcb);
        ncb->lb_cpy_offset_ = cpcb;

        /* 清理普通包缓冲区的描述信息 */
        ncb->recv_analyze_offset_ = 0;

        /* 大包构建阶段，单次接收缓冲区的数据肯定会一次用尽 */
        return 0;
    }

    /*剩余的字节数，足以构造整个包*/
    if ((ncb->recv_analyze_offset_ + overplus) >= total_packet_length) {
        memcpy(ncb->packet_ + ncb->recv_analyze_offset_, cpbuff, total_packet_length - ncb->recv_analyze_offset_);

        /*返回给上级调用的字节数=本次的剩余字节数-构建本包总共消耗的字节数*/
        retcb = (overplus - (total_packet_length - ncb->recv_analyze_offset_));

        /*完成组包, 回调给上层模块*/
        c_event.Ln.Tcp.Link = ncb->hld_;
        c_event.Event = EVT_RECEIVEDATA;
        c_data.e.Packet.Data = (ncb->packet_ + ncb->tst_.cb_);
        c_data.e.Packet.Size = user_data_size;
        if (ncb->user_callback_) {
            ncb->user_callback_(&c_event, &c_data);
        }

        ncb->recv_analyze_offset_ = 0;
        return retcb;
    }

    /*剩余字节数不足以构造一个完整包， 则把剩余字节数都放入缓冲区，并调整包解析偏移*/
    memmove(ncb->packet_ + ncb->recv_analyze_offset_, cpbuff, overplus);
    ncb->recv_analyze_offset_ += overplus;
    return 0;
}