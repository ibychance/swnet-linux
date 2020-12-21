#include "nis.h"
#include "posix_wait.h"
#include "posix_ifos.h"
#include "posix_naos.h"
#include "posix_atomic.h"
#include "logger.h"

#include "args.h"
#include "tst.h"

int display(HTCPLINK link, const unsigned char *data, int size)
{
	char output[1024];
	uint32_t ip;
	uint16_t port;
	char ipstr[INET_ADDRSTRLEN];
	int offset;

	tcp_getaddr(link, LINK_ADDR_REMOTE, &ip, &port);
	posix__ipv4tos(ip, ipstr, sizeof(ipstr));
	offset = sprintf(output, "[income %s:%u] ", ipstr, port);

	if (size < (sizeof(output) - offset) && size > 0) {
		memcpy(&output[offset], data, size);
		return posix__file_write(1, output, size + offset);
	}

	return -1;
}

void on_server_receive_data(HTCPLINK link, const unsigned char *data, int size)
{
	do {
		if (size <= 1024) {
			if (display(link, data, size) <=0 ) {
				break;
			}
		}

		if (tcp_write(link, data, size, NULL) < 0) {
			//break;
		}

		return;
	} while (0);

	tcp_destroy(link);
}

void STDCALL tcp_server_callback(const struct nis_event *event, const void *data)
{
	struct nis_tcp_data *tcpdata;
	HTCPLINK link;

	tcpdata = (struct nis_tcp_data *)data;
	link = event->Ln.Tcp.Link;
	switch(event->Event) {
		case EVT_RECEIVEDATA:
			on_server_receive_data(link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size);
			break;
		case EVT_TCP_ACCEPTED:
		case EVT_CLOSED:
		default:
			break;
	}
}

void STDCALL tcp_client_callback(const struct nis_event *event, const void *data)
{
	struct nis_tcp_data *tcpdata = (struct nis_tcp_data *)data;

	switch(event->Event) {
		case EVT_RECEIVEDATA:
			display(event->Ln.Tcp.Link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size);
			posix__file_write(1, "input:$ ", 8);
			break;
		case EVT_TCP_CONNECTED:
			posix__file_write(1, "input:$ ", 8);
			break;
		case EVT_TCP_ACCEPTED:
		case EVT_CLOSED:
		default:
			break;
	}
}

void STDCALL nshost_ecr(const char *host_event, const char *reserved, int rescb)
{
	if (host_event) {
		ECHO("echo", "%s", host_event);
	}
}

int echo_server_startup(const char *host, uint16_t port)
{
	HTCPLINK server;
	tst_t tst;
	int attr;

	server = tcp_create(&tcp_server_callback, host, port);
	if (INVALID_HTCPLINK == server) {
		return 1;
	}

	tst.parser_ = &nsp__tst_parser;
	tst.builder_ = &nsp__tst_builder;
	tst.cb_ = sizeof(nsp__tst_head_t);
	nis_cntl(server, NI_SETTST, &tst);

	attr = nis_cntl(server, NI_GETATTR);
    if (attr >= 0 ) {
    	attr |=	LINKATTR_TCP_UPDATE_ACCEPT_CONTEXT;
    	attr = nis_cntl(server, NI_SETATTR, attr);
    }

	tcp_listen(server, 100);
	posix__hang();
	return 0;
}

int echo_client_startup(const char *host, uint16_t port)
{
	HTCPLINK client;
	char text[65535], *p;
	size_t n;

	do {
		client = tcp_create(&tcp_client_callback, NULL, 0);
		if (INVALID_HTCPLINK == client) {
			break;
		}

		if (tcp_connect2(client, host, port) < 0) {
			break;
		}

		while ( NULL != (p = fgets(text, sizeof(text), stdin)) ) {
			n = strlen(text);
			if ( n > 0) {
				if (tcp_write(client, text, n, NULL) < 0) {
					break;
				}
			}
		}
	} while( 0 );

	return 1;
}

int main(int argc, char **argv)
{
	int type;

	if (check_args(argc, argv) < 0) {
		return -1;
	}

	if ((type = gettype()) < 0 ) {
		return 1;
	}

	log__init();
	tcp_init();
	nis_checr(&nshost_ecr);

	if (type == SESS_TYPE_SERVER) {
		return echo_server_startup(gethost(), getport());
	}

	if (type == SESS_TYPE_CLIENT) {
		return echo_client_startup(gethost(), getport());
	}

	return 0;
}
