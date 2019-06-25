#include "nis.h"
#include "posix_naos.h"

#include <stdio.h>

#include <unistd.h>

void on_arp_callback(const struct nis_event *event, const void *data)
{
	const struct nis_arp_data *parp;
	int i;
	char ipstr[16];

	if (event->Event == EVT_RECEIVEDATA) {
		parp = (const struct nis_arp_data *)data;

		posix__ipv4tos(parp->e.Packet.Arp_Sender_Ip, ipstr, sizeof(ipstr));
		printf("sender IP = %s\n", ipstr);
		for (i = 0; i < 6; i++) {
			printf("%02x ", parp->e.Packet.Arp_Sender_Mac[i]);
		}
		printf("\n");
	}
}

int main(int argc, char **argv)
{
	HARPLINK link;

	udp_init();

	link = arp_create(&on_arp_callback, "10.10.100.253");
	if (INVALID_HUDPLINK == link)
		return 1;

	arp_request(link, "10.10.100.153");

	while(1)
		sleep(1);
	return 0;
}
