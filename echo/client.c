#include "nis.h"
#include "posix_thread.h"
#include "posix_wait.h"
#include "posix_ifos.h"

#include "args.h"
#include "tst.h"

struct nstest_client_context
{
    int echo;
    char *host;
    uint16_t port;
    int interval;
    int threads;
    int length;
} __context = { 0 };

static void nstest_echo_client(HTCPLINK link)
{
    char *p;
    char text[65536];
    size_t n;

    posix__file_write(STDOUT_FILENO, "input:$ ", 8);

    while ( NULL != (p = fgets(text, sizeof(text), stdin)) ) {
        n = strlen(text);
        if ( n > 0) {
            if (tcp_write(link, text, n, NULL) < 0) {
                break;
            }
        }
    }
}

extern int display(HTCPLINK link, const unsigned char *data, int size);
static void STDCALL tcp_client_callback(const struct nis_event *event, const void *data)
{
    struct nis_tcp_data *tcpdata = (struct nis_tcp_data *)data;

    switch(event->Event) {
        case EVT_RECEIVEDATA:
            if (__context.echo) {
                display(event->Ln.Tcp.Link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size);
                posix__file_write(STDOUT_FILENO, "input:$ ", 8);
            }
            break;
        case EVT_TCP_ACCEPTED:
        case EVT_CLOSED:
        default:
            break;
    }
}

static void *nstest_sender_routine(void *parameter)
{
    HTCPLINK link;
    char *p;

    link = *(HTCPLINK *)parameter;
    p = (char *)malloc(__context.length);
    assert(p);

    while (1) {
        tcp_write(link, p, __context.length, NULL);
        posix__delay_execution(__context.interval * 1000);
    }

    free(p);
    return NULL;
}

int nstest_client_startup()
{
    HTCPLINK client;
    tst_t tst;
    int i;
    posix__pthread_t *tidps;
    void *thret;

    getclientcontext(&__context.host, &__context.port, &__context.echo,
        &__context.interval, &__context.threads, &__context.length);

    tidps = NULL;

    do {
        /* nail the TST and allow accept update */
        tst.parser_ = &nsp__tst_parser;
        tst.builder_ = &nsp__tst_builder;
        tst.cb_ = sizeof(nsp__tst_head_t);
        nis_cntl(client, NI_SETTST, &tst);
        client = tcp_create2(&tcp_client_callback, NULL, 0, &tst);
        if (INVALID_HTCPLINK == client) {
            break;
        }

        if (tcp_connect(client, __context.host, __context.port) < 0) {
            break;
        }

        /* into echo model */
        if (__context.echo) {
            nstest_echo_client(client);
            return 0;
        }

        tidps = (posix__pthread_t *)malloc(__context.threads * sizeof(posix__pthread_t));
        assert(tidps);
        for ( i = 0; i < __context.threads; i++) {
            posix__pthread_create(&tidps[i],&nstest_sender_routine, (void *)&client);
        }

        for ( i = 0; i < __context.threads; i++) {
            posix__pthread_join(&tidps[i], &thret);
        }

    } while( 0 );

    if (tidps) {
        free(tidps);
        return 0;
    }
    return 1;
}
