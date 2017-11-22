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
    if (0 == ncb->template.cb_) {
        c_event.Ln.Tcp.Link = (HTCPLINK) ncb->hld;
        c_event.Event = EVT_RECEIVEDATA;
        c_data.e.Packet.Size = cpcb;
        c_data.e.Packet.Data = (const char *) ((char *) data);
        if (ncb->nis_callback) {
            ncb->nis_callback(&c_event, &c_data);
        }
        ncb->rx_parse_offset = 0;
        return 0;
    }

    /*�Ѿ������Ϊ�������״̬ ������������*/
    if (ncb_lb_marked(ncb)) {
        /*���ε��������в������������*/
        if (cpcb + ncb->lboffset < ncb->lbsize) {
            memcpy(ncb->lbdata + ncb->lboffset, cpbuff, cpcb);
            ncb->lboffset += cpcb;
            return 0;
        }

        /*���ε�������������ɴ��*/
        overplus = ncb->lbsize - ncb->lboffset;
        memcpy(ncb->lbdata + ncb->lboffset, cpbuff, overplus);

        /*������, �ص����ϲ�ģ��*/
        if (ncb->nis_callback) {
            c_event.Ln.Tcp.Link = ncb->hld;
            c_event.Event = EVT_RECEIVEDATA;
            c_data.e.Packet.Data = (ncb->lbdata + ncb->template.cb_);
            c_data.e.Packet.Size = ncb->lbsize - ncb->template.cb_;
            ncb->nis_callback(&c_event, &c_data);
        }

        /*�ͷŴ��������*/
        free(ncb->lbdata);
        ncb->lbdata = NULL;
        ncb->lboffset = 0;
        ncb->lbsize = 0;
        return (cpcb - overplus);
    }

    /*���ݳ����в��㹹��Э��ͷ, ȫ���������ڹ���Э��ͷ�� ����ʣ�೤��0*/
    if (ncb->rx_parse_offset + cpcb < ncb->template.cb_) {
        memcpy(ncb->packet + ncb->rx_parse_offset, cpbuff, cpcb);
        ncb->rx_parse_offset += cpcb;
        return 0;
    }

    overplus = cpcb;
    used = 0;

    /*��ǰ���е����ݲ����Թ���Э��ͷ�����Ǽ��ϱ������ݣ����Թ���Э��ͷ*/
    if (ncb->rx_parse_offset < ncb->template.cb_) {
        used += (ncb->template.cb_ - ncb->rx_parse_offset);
        overplus = cpcb - used;
        memcpy(ncb->packet + ncb->rx_parse_offset, cpbuff, used);
        cpbuff += used;
        ncb->rx_parse_offset = ncb->template.cb_;
    }

    /* �ײ�Э�齻����Э��ģ�崦�� ����ʧ�����������޷����� */
    if (!(*ncb->template.parser_)) {
        ncb_report_debug_information(ncb, "[TCP]invalidated link object TST parser function address.\n");
        return -1;
    }

	/* ͨ���������̵õ��û������ݳ��� */
    if ((*ncb->template.parser_)(ncb->packet, ncb->rx_parse_offset, &user_data_size) < 0) {
		return -1;
	}
	
	/* ����û����ݳ��ȳ���������̳��ȣ���ֱ�ӱ���Ϊ����, �п����Ƕ��⹥�� */
    if (user_data_size > TCP_MAXIMUM_PACKET_SIZE) {
		return -1;
	}
	
    /* ����ͷ�İ��ܳ��� */
    total_packet_length = user_data_size + ncb->template.cb_;

    /* ����Ǵ������Ӧ�ý���������� */
    if (total_packet_length > TCP_BUFFER_SIZE) {
        ncb->lbdata = (char *) malloc(total_packet_length);
        if (!ncb->lbdata) {
            return -1;
        }

        /* ���ײ�Э���ͷ�Ĵ�������ܳ��� */
        ncb->lbsize = total_packet_length;

        /* ����ȫ�����ݿ�������������� */
        memcpy(ncb->lbdata, data, cpcb);
        ncb->lboffset = cpcb;

        /* ������ͨ����������������Ϣ */
        ncb->rx_parse_offset = 0;

        /* ��������׶Σ����ν��ջ����������ݿ϶���һ���þ� */
        return 0;
    }

    /*ʣ����ֽ��������Թ���������*/
    if ((ncb->rx_parse_offset + overplus) >= total_packet_length) {
        memcpy(ncb->packet + ncb->rx_parse_offset, cpbuff, total_packet_length - ncb->rx_parse_offset);

        /*���ظ��ϼ����õ��ֽ���=���ε�ʣ���ֽ���-���������ܹ����ĵ��ֽ���*/
        retcb = (overplus - (total_packet_length - ncb->rx_parse_offset));

        /*������, �ص����ϲ�ģ��*/
        c_event.Ln.Tcp.Link = ncb->hld;
        c_event.Event = EVT_RECEIVEDATA;
        c_data.e.Packet.Data = (ncb->packet + ncb->template.cb_);
        c_data.e.Packet.Size = user_data_size;
        if (ncb->nis_callback) {
            ncb->nis_callback(&c_event, &c_data);
        }

        ncb->rx_parse_offset = 0;
        return retcb;
    }

    /*ʣ���ֽ��������Թ���һ���������� ���ʣ���ֽ��������뻺������������������ƫ��*/
    memmove(ncb->packet + ncb->rx_parse_offset, cpbuff, overplus);
    ncb->rx_parse_offset += overplus;
    return 0;
}