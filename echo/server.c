#include "nis.h"
#include "posix_wait.h"

#include "args.h"
#include "tst.h"

struct nstest_server_context
{
    int echo;
    int mute;
    uint16_t port;
};

extern int display(HTCPLINK link, const unsigned char *data, int size);

static void on_server_receive_data(HTCPLINK link, const unsigned char *data, int size)
{
    struct nstest_server_context *context;

    nis_cntl(link, NI_GETCTX, &context);

    do {
        if (context->echo) {
            display(link, data, size);
            return;
        }

        if (!context->mute) {
            tcp_write(link, data, size, NULL);
        } else {
            tcp_write(link, "mute", 4, NULL);
        }

        return;
    } while (0);

    tcp_destroy(link);
}

static void on_server_accepted(HTCPLINK local, HTCPLINK accepted)
{
    struct nstest_server_context *context, *update;

    nis_cntl(local, NI_GETCTX, &context);
    update =  (struct nstest_server_context *)malloc(sizeof(struct nstest_server_context));
    assert(update);
    memcpy(update, context, sizeof(struct nstest_server_context));
    nis_cntl(accepted, NI_SETCTX, update);
}

static void STDCALL tcp_server_callback(const struct nis_event *event, const void *data)
{
    struct nis_tcp_data *tcpdata;

    tcpdata = (struct nis_tcp_data *)data;
    switch(event->Event) {
        case EVT_RECEIVEDATA:
            on_server_receive_data(event->Ln.Tcp.Link, tcpdata->e.Packet.Data, tcpdata->e.Packet.Size);
            break;
        case EVT_TCP_ACCEPTED:
            on_server_accepted(event->Ln.Tcp.Link, tcpdata->e.Accept.AcceptLink);
            break;
        case EVT_PRE_CLOSE:
            assert(tcpdata->e.PreClose.Context);
            free((struct nstest_server_context *)tcpdata->e.PreClose.Context);
            break;
        case EVT_CLOSED:
        default:
            break;
    }
}

int nstest_server_startup()
{
    HTCPLINK server;
    tst_t tst;
    struct nstest_server_context *context;
    int attr;

    context = (struct nstest_server_context *)malloc(sizeof(struct nstest_server_context));
    assert(context);

    /* load argument from startup parameters */
    getservercontext(&context->echo, &context->mute, &context->port);

    /* create TCP link */
    server = tcp_create(&tcp_server_callback, NULL, context->port);
    if (INVALID_HTCPLINK == server) {
        return 1;
    }

    /* nail the context to listen link */
    nis_cntl(server, NI_SETCTX, context);

    /* nail the TST and allow accept update */
    tst.parser_ = &nsp__tst_parser;
    tst.builder_ = &nsp__tst_builder;
    tst.cb_ = sizeof(nsp__tst_head_t);
    nis_cntl(server, NI_SETTST, &tst);

    attr = nis_cntl(server, NI_GETATTR);
    if (attr >= 0 ) {
        attr |= LINKATTR_TCP_UPDATE_ACCEPT_CONTEXT;
        attr = nis_cntl(server, NI_SETATTR, attr);
    }

    /* begin listen */
    tcp_listen(server, 100);
    /* block the program auto exit */
    posix__hang();
    return 0;
}
