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
    kOptIndex_Target = 't',
    kOptIndex_SetPort = 'p',
    kOptIndex_Server = 's',
    kOptIndex_Client = 'c',
};

static const struct option long_options[] = {
    {"help", no_argument, NULL, kOptIndex_GetHelp},
    {"version", no_argument, NULL, kOptIndex_GetVersion},
    {"port", required_argument, NULL, kOptIndex_SetPort},
    {"server", no_argument, NULL, kOptIndex_Server},
    {"client", no_argument, NULL, kOptIndex_Client},
    {NULL, 0, NULL, 0}
};

void display_usage()
{
    static const char *usage_context =
            "usage: nshost.echo [-s|-c host] [options]\tnshost.echo {-v|--version|-h|--help}\n"
            "\t-p, --port\tchange TCP port, effective either client or server, 10256 by default\n"
            ;

    printf("%s", usage_context);
}

static void display_author_information()
{
    static const char *author_context =
            "nshost.echo 1,1,0,0\n"
            "Copyright (C) 2017 Jerry.Anderson\n"
            "For bug reporting instructions, please see:\n"
            "<http://www.nsplibrary.com.cn/>.\n"
            "For help, type \"help\".\n"
            ;
    printf("%s", author_context);
}

int check_args(int argc, char **argv)
{
    int opt_index;
    int opt;
    int retval = 0;
    char shortopts[128];

    __startup_parameters.type = SESS_TYPE_SERVER;
    strcpy(__startup_parameters.host, "0.0.0.0");
    __startup_parameters.port = 10256;

    /* double '::' meat option may have argument or not,
        one ':' meat option MUST have argument,
        no ':' meat option MUST NOT have argument */
    strcpy(shortopts, "s::c::vhp:");
    opt = getopt_long(argc, argv, shortopts, long_options, &opt_index);
    while (opt != -1) {
        switch (opt) {
            case 's':
            case 'c':
                if (optarg) {
                    strcpy(__startup_parameters.host, optarg);
                }
                __startup_parameters.type = opt;
                break;
            case 'v':
                display_author_information();
                return -1;
            case 'h':
                display_usage();
                return -1;
            case 'p':
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
