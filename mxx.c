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
    version->revision_ = 6;
    nis_call_ecr("nshost.mxx:version %d.%d.%d", version->major_, version->minor_, version->revision_);
    return 0;
}

char *nis_lgethost(char *name, int cb) {
    if (name && cb > 0) {
        if (0 == gethostname(name, cb)) {
            return name;
        } else {
            nis_call_ecr("nshost.mxx:fail syscall gethostname, error:%u", errno);
        }
    }
    return name;
}

int nis_gethost(const char *name, uint32_t *ipv4) {
    struct hostent *remote, ret;
    struct in_addr addr;
    int h_errnop;
    char buf[1024];

    if (!name || !ipv4) {
        return -EINVAL;
    }
    
    *ipv4 = 0;
    remote = NULL;

    if (isalpha(name[0])) { /* host address is a name */
        gethostbyname_r(name, &ret, buf, sizeof(buf), &remote, &h_errnop);
    } else {
        /* 
        inet_aton() converts the Internet host address cp from the IPv4 numbers-and-dots notation into binary form (in network byte order) 
                    and stores it in the structure that inp points to.  
        inet_aton() returns nonzero if the address is valid, zero if not.  The address supplied in cp can have one of the following forms:
        a.b.c.d   Each of the four numeric parts specifies a byte of the address; the bytes are assigned in left-to-right order to produce the binary address.
        a.b.c     Parts a and b specify the first two bytes of the binary address.  
                 Part c is interpreted as a 16-bit value that defines the rightmost two bytes of the binary address.  
                 This  notation  is  suitable  for  specifying  (outmoded)  Class  B  network addresses.
        a.b       Part a specifies the first byte of the binary address.  Part b is interpreted as a 24-bit value that defines the rightmost three bytes of the binary address.  This notation is suitable for specifying (outmoded) Class A network addresses.
        a         The value a is interpreted as a 32-bit value that is stored directly into the binary address without any byte rearrangement.
        In  all  of  the  above forms, components of the dotted address can be specified in decimal, octal (with a leading 0), or hexadecimal, with a leading 0X).  
        Addresses in any of these forms are collectively termed IPV4 numbers-and-dots notation.  
        The form that uses exactly four decimal numbers is referred to as IPv4 dotted-decimal notation (or sometimes: IPv4 dotted-quad notation).
        inet_aton() returns 1 if the supplied string was successfully interpreted, or 0 if the string is invalid (errno is not set on error).
        */
        if (inet_aton(name, &addr)) {
            gethostbyaddr_r(&addr, sizeof(addr), AF_INET, &ret, buf, sizeof(buf), &remote, &h_errnop);
        }
    }

    if (!remote) {
        return -1;
    }
    
    /* only IPv4 protocol supported */
    if (AF_INET != remote->h_addrtype) {
        return -EPROTONOSUPPORT;
    }

    if (!remote->h_addr_list) {
        return -ENOENT;
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