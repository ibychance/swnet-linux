#include "args.h"

#include <getopt.h>

static struct {
    int type;
    char host[128];
    uint16_t port;
} __startup_parameters;

enum ope_index {
    kOptIndex_GetHelp = 'h',
    kOptIndex_GetVersion = 'v',
    kOptIndex_SetHost = 'H',
    kOptIndex_SetPort = 'P',
    kOptIndex_Server = 'S',
    kOptIndex_Client = 'C',
};

static const struct option long_options[] = {
    {"help", no_argument, NULL, kOptIndex_GetHelp},
    {"host", required_argument, NULL, kOptIndex_SetHost},
    {"version", no_argument, NULL, kOptIndex_GetVersion},
    {"port", required_argument, NULL, kOptIndex_SetPort},
    {"server", no_argument, NULL, kOptIndex_Server},
    {"client", no_argument, NULL, kOptIndex_Client},
    {NULL, 0, NULL, 0}
};

void display_usage()
{
    static const char *usage_context =
            "usage escape - nshost escape timing counter\n"
            "SYNOPSIS\n"
            "\t[-h|--help]\tdisplay usage context and help informations\n"
            "\t[-v|--version]\tdisplay versions of executable archive\n"
            "\t[-S|--server]\tthis session run as a server.\n"
            "\t[-C|--client]\tthis session run as a client.this option is default.\n"
            "\t[-H|--host]\ttarget server IPv4 address or domain when @C or local bind adpater when @S, \"0.0.0.0\" by default\n"
            "\t[-P|--port]\ttarget server TCP port when @C or local listen TCP port when @S, 10256 by default\n"
            ;

    printf("%s", usage_context);
}

static void display_author_information()
{
    static const char *author_context =
            "nshost echo 1,1,0,0\n"
            "Copyright (C) 2017 Jerry.Anderson\n"
            "For bug reporting instructions, please see:\n"
            "<http://www.nshost.com.cn/>.\n"
            "For help, type \"help\".\n"
            ;
    printf("%s", author_context);
}

int check_args(int argc, char **argv) {
    int opt_index;
    int opt;
    int retval = 0;
    char shortopts[128];

    __startup_parameters.type = SESS_TYPE_CLIENT;
    strcpy(__startup_parameters.host, "0.0.0.0");
    __startup_parameters.port = 10256;

    strcpy(shortopts, "SCvhH:P:");
    opt = getopt_long(argc, argv, shortopts, long_options, &opt_index);
    while (opt != -1) {
        switch (opt) {
            case 'S':
                __startup_parameters.type = SESS_TYPE_SERVER;
                break;
            case 'C':
                __startup_parameters.type = SESS_TYPE_CLIENT;
                break;
            case 'v':
                display_author_information();
                return -1;
            case 'h':
                display_usage();
                return -1;
            case 'H':
                strcpy(__startup_parameters.host, optarg);
                break;
            case 'P':
                __startup_parameters.port = (uint16_t) strtoul(optarg, NULL, 10);
                break;
            case '?':
                printf("?\n");
            case 0:
                printf("0\n");
            default:
                display_usage();
                return -1;
        }
        opt = getopt_long(argc, argv, shortopts, long_options, &opt_index);
    }

	if ( __startup_parameters.type == SESS_TYPE_CLIENT && 0 == strcasecmp( __startup_parameters.host, "0.0.0.0" ) ) {
		display_usage();
		return -1;
	}

    return retval;
}

int gettype()
{
    return __startup_parameters.type;
}

const char *gethost()
{
    return &__startup_parameters.host[0];
}

uint16_t getport()
{
    return __startup_parameters.port;
}
