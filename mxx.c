/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */
#include "mxx.h"

#include <ctype.h>
#include <stdarg.h>
#include <netdb.h>

#include "ncb.h"

int nis_getver(swnet_version_t *version) {
    if (!version) {
        return -1;
    }

    version->major_ = 9;
    version->minor_ = 7;
    version->revision_ = 1;
    nis_call_ecr("nshost version %d.%d.%d", version->major_, version->minor_, version->revision_);
    return 0;
}

char *nis_lgethost(char *name, int cb) {
    if (name && cb > 0) {
        if (0 == gethostname(name, cb)) {
            return name;
        }
    }
    return name;
}

int nis_gethost(const char *name, uint32_t *ipv4) {
    struct hostent *remote;
    struct in_addr addr;

    if (!name || !ipv4) {
        return -1;
    }
    
    *ipv4 = 0;

    if (isalpha(name[0])) { /* host address is a name */
        remote = gethostbyname(name);
    } else {
        addr.s_addr = inet_addr(name);
        if (INADDR_NONE == addr.s_addr) {
            return -1;
        } else {
            remote = gethostbyaddr((char *) &addr, 4, AF_INET);
        }
    }

    if (!remote) {
        return -1;
    }
    
    /* only IPv4 protocol supported */
    if (AF_INET != remote->h_addrtype) {
        return -1;
    }

    if (!remote->h_addr_list) {
        return -1;
    }

    if (remote->h_length < sizeof (uint32_t)) {
        return -1;
    }

    addr.s_addr = *((uint32_t *) remote->h_addr_list[0]);
    *ipv4 = ntohl(addr.s_addr);
    return 0;
}

/* manage ECR and it's calling */
static nis_event_callback_t current_ecr = NULL;

nis_event_callback_t nis_checr(const nis_event_callback_t ecr) {
    if (!ecr) {
        __sync_lock_release(&current_ecr);
        return NULL;
    }
    return __sync_lock_test_and_set(&current_ecr, ecr);
}

void nis_call_ecr(const char *fmt,...) {
    nis_event_callback_t ecr = NULL;
    nis_event_callback_t old;
    va_list ap;
    char logstr[1280]; 
    int retval;

    if (!current_ecr) {
        return;
    }

    va_start(ap, fmt);
    retval = vsnprintf(logstr, cchof(logstr) - 1, fmt, ap);
    va_end(ap);
    if (retval <= 0) {
        return;
    }
    logstr[retval] = 0;

    /* double check the callback address */
    old = __sync_lock_test_and_set(&ecr, current_ecr);
    if (ecr && !old) {
        ecr(logstr, NULL, 0);
    }
}