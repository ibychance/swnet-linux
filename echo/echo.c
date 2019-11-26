#include "args.h"

#include "nis.h"
#include "posix_wait.h"
#include "posix_ifos.h"
#include "logger.h"

#include <stdio.h>

/*#define echo_logger_info(context) log__save("nshost.echo", kLogLevel_Info, kLogTarget_Stdout | kLogTarget_Filesystem, #context)*/

void nshost_tcp_callback(const struct nis_event *event, const void *data)
{
	int n;
	int size;
	char output[1024];
	struct nis_tcp_data *tcpdata = (struct nis_tcp_data *)data;
	HTCPLINK link;

	link = event->Ln.Tcp.Link;
	switch(event->Event) {
		case EVT_RECEIVEDATA:
			size = tcpdata->e.Packet.Size;
			if (size < 1020 && size > 0) {
				memcpy(output, ">>> ", 4);
				memcpy(output + 4, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size);
				output[tcpdata->e.Packet.Size + 4] = 0;
				n = 4 + tcpdata->e.Packet.Size + 1;
				posix__file_write(1,output, n);
				if (tcp_write(link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size, NULL) <= 0) {
					tcp_destroy(link);
				}
			} else {
				tcp_destroy(link);
			}
			break;
		case EVT_TCP_ACCEPTED:
			break;
		case EVT_CLOSED:
			break;
		case EVT_TCP_CONNECTED:
			log__save("nshost.echo", kLogLevel_Info, kLogTarget_Stdout | kLogTarget_Filesystem,"client link "UINT64_STRFMT" connected.", link);
			break;
		default:
			break;
	}
}

void nshost_ecr(const char *host_event, const char *reserved, int rescb)
{
	if (host_event) {
		log__save("nshost.echo", kLogLevel_Info, kLogTarget_Stdout | kLogTarget_Filesystem, "%s", host_event);
	}
}

int echo_server_startup(const char *host, uint16_t port)
{
	HTCPLINK server;

	log__save("nshost.echo", kLogLevel_Info, kLogTarget_Stdout | kLogTarget_Filesystem, "server host %s:%u", host, port);

	server = tcp_create(&nshost_tcp_callback, host, port);
	if (INVALID_HTCPLINK == server) {
		return 1;
	}

	tcp_listen(server, 100);

	posix__hang();
	return 0;
}

int echo_client_startup(const char *host, uint16_t port)
{
	int i;
	HTCPLINK client[2];

	log__save("nshost.echo", kLogLevel_Info, kLogTarget_Stdout | kLogTarget_Filesystem, "clinet target host %s:%u", host, port);

	for (i = 0; i < 2; i++) {
		client[i] = tcp_create(&nshost_tcp_callback, NULL, 0);
		if (INVALID_HTCPLINK == client[i]) {
			return 1;
		}
		log__save("nshost.echo", kLogLevel_Info, kLogTarget_Stdout | kLogTarget_Filesystem,"client link "UINT64_STRFMT" created.", client[i]);
	}

	for (i = 0; i < 2; i++) {
		tcp_connect2(client[i], host, port);
	}

	posix__hang();
	return 0;
}

int main(int argc, char **argv)
{
	int type;

	if (check_args(argc, argv) < 0) {
		return -1;
	}

	if ((type = gettype()) < 0 ){
		return 1;
	}

	log__init();
	nis_checr(&nshost_ecr);
	tcp_init();

	if (type == SESS_TYPE_SERVER) {
		return echo_server_startup(gethost(), getport());
	}

	if (type == SESS_TYPE_CLIENT) {
		return echo_client_startup(gethost(), getport());
	}

	return 0;
}
