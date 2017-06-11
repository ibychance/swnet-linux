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

    /* û��ָ����ͷģ�壬 ֱ�ӻص�����TCP�� */
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

    /*�Ѿ������Ϊ�������״̬ ������������*/
    if (ncb_lb_marked(ncb)) {
        /*���ε��������в������������*/
        if (cpcb + ncb->lb_cpy_offset_ < ncb->lb_length_) {
            memcpy(ncb->lb_data_ + ncb->lb_cpy_offset_, cpbuff, cpcb);
            ncb->lb_cpy_offset_ += cpcb;
            return 0;
        }

        /*���ε�������������ɴ��*/
        overplus = ncb->lb_length_ - ncb->lb_cpy_offset_;
        memcpy(ncb->lb_data_ + ncb->lb_cpy_offset_, cpbuff, overplus);

        /*������, �ص����ϲ�ģ��*/
        if (ncb->user_callback_) {
            c_event.Ln.Tcp.Link = ncb->hld_;
            c_event.Event = EVT_RECEIVEDATA;
            c_data.e.Packet.Data = (ncb->lb_data_ + ncb->tst_.cb_);
            c_data.e.Packet.Size = ncb->lb_length_ - ncb->tst_.cb_;
            ncb->user_callback_(&c_event, &c_data);
        }

        /*�ͷŴ��������*/
        free(ncb->lb_data_);
        ncb->lb_data_ = NULL;
        ncb->lb_cpy_offset_ = 0;
        ncb->lb_length_ = 0;
        return (cpcb - overplus);
    }

    /*���ݳ����в��㹹��Э��ͷ, ȫ���������ڹ���Э��ͷ�� ����ʣ�೤��0*/
    if (ncb->recv_analyze_offset_ + cpcb < ncb->tst_.cb_) {
        memcpy(ncb->packet_ + ncb->recv_analyze_offset_, cpbuff, cpcb);
        ncb->recv_analyze_offset_ += cpcb;
        return 0;
    }

    overplus = cpcb;
    used = 0;

    /*��ǰ���е����ݲ����Թ���Э��ͷ�����Ǽ��ϱ������ݣ����Թ���Э��ͷ*/
    if (ncb->recv_analyze_offset_ < ncb->tst_.cb_) {
        used += (ncb->tst_.cb_ - ncb->recv_analyze_offset_);
        overplus = cpcb - used;
        memcpy(ncb->packet_ + ncb->recv_analyze_offset_, cpbuff, used);
        cpbuff += used;
        ncb->recv_analyze_offset_ = ncb->tst_.cb_;
    }

    /* �ײ�Э�齻����Э��ģ�崦�� ����ʧ�����������޷����� */
    if (!(*ncb->tst_.parser_)) {
        ncb_report_debug_information(ncb, "[TCP]invalidated link object TST parser function address.\n");
        return -1;
    }

    if ((*ncb->tst_.parser_)(ncb->packet_, ncb->recv_analyze_offset_, &user_data_size) < 0) return -1;
    if (user_data_size > TCP_MAXIMUM_PACKET_SIZE) return -1;

    /* ����ͷ�İ��ܳ��� */
    total_packet_length = user_data_size + ncb->tst_.cb_;

    /* ����Ǵ������Ӧ�ý���������� */
    if (total_packet_length > TCP_BUFFER_SIZE) {
        ncb->lb_data_ = (char *) malloc(total_packet_length);
        if (!ncb->lb_data_) {
            return -1;
        }

        /* ���ײ�Э���ͷ�Ĵ�������ܳ��� */
        ncb->lb_length_ = total_packet_length;

        /* ����ȫ�����ݿ�������������� */
        memcpy(ncb->lb_data_, data, cpcb);
        ncb->lb_cpy_offset_ = cpcb;

        /* ������ͨ����������������Ϣ */
        ncb->recv_analyze_offset_ = 0;

        /* ��������׶Σ����ν��ջ����������ݿ϶���һ���þ� */
        return 0;
    }

    /*ʣ����ֽ��������Թ���������*/
    if ((ncb->recv_analyze_offset_ + overplus) >= total_packet_length) {
        memcpy(ncb->packet_ + ncb->recv_analyze_offset_, cpbuff, total_packet_length - ncb->recv_analyze_offset_);

        /*���ظ��ϼ����õ��ֽ���=���ε�ʣ���ֽ���-���������ܹ����ĵ��ֽ���*/
        retcb = (overplus - (total_packet_length - ncb->recv_analyze_offset_));

        /*������, �ص����ϲ�ģ��*/
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

    /*ʣ���ֽ��������Թ���һ���������� ���ʣ���ֽ��������뻺������������������ƫ��*/
    memmove(ncb->packet_ + ncb->recv_analyze_offset_, cpbuff, overplus);
    ncb->recv_analyze_offset_ += overplus;
    return 0;
}