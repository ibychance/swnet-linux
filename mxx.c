/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */
#include "mxx.h"

#include <ctype.h>
#include <stdarg.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <arpa/inet.h>

#include "nisdef.h"
#include "ncb.h"

#include "tcp.h"
#include "udp.h"

int nis_getver(swnet_version_t *version)
{
    if (!version) {
        return -1;
    }

    version->major_ = 9;
    version->minor_ = 7;
    version->revision_ = 6;
    nis_call_ecr("[nshost.mxx.nis_getver] current version %d.%d.%d", version->major_, version->minor_, version->revision_);
    return 0;
}

char *nis_lgethost(char *name, int cb)
{
    if (name && cb > 0) {
        if (0 == gethostname(name, cb)) {
            return name;
        } else {
            nis_call_ecr("[nshost.mxx.nis_lgethost] fatal error occurred syscall gethostname(2), error:%u", errno);
        }
    }
    return name;
}

int nis_gethost(const char *name, uint32_t *ipv4)
{
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

nis_event_callback_t nis_checr(const nis_event_callback_t ecr)
{
    if (!ecr) {
        __sync_lock_release(&current_ecr);
        return NULL;
    }
    return __sync_lock_test_and_set(&current_ecr, ecr);
}

/* the ecr usually use for diagnose low-level error */
void nis_call_ecr(const char *fmt,...)
{
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

int nis_getifmisc(ifmisc_t *ifv, int *cbifv)
{
    struct ifaddrs *ifa, *ifs;
    int count;
    int i;
    int cbacquire;

    ifa = NULL;
    count = 0;

    if (!cbifv) {
        return -EINVAL;
    }

    if (*cbifv > 0 && !ifv) {
        return -EINVAL;
    }

    if (getifaddrs(&ifs) < 0) {
        return posix__makeerror(errno);
    }

    for (ifa = ifs; ifa != NULL; ifa = ifa->ifa_next) {
        if(ifa->ifa_addr->sa_family == AF_INET) {
            ++count;
        }
    }

    cbacquire = count * sizeof(ifmisc_t);
    if (*cbifv < cbacquire) {
        *cbifv = cbacquire;
        return -EAGAIN;
    }

    i = 0;
    for (ifa = ifs; ifa != NULL; ifa = ifa->ifa_next) {
        if(ifa->ifa_addr->sa_family == AF_INET) {
            strncpy(ifv[i].interface_, ifa->ifa_name, sizeof(ifv[i].interface_) - 1);
            ifv[i].addr_ = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
            ifv[i].netmask_ = ((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr;
            ifv[i].boardcast_ = ((struct sockaddr_in *)ifa->ifa_ifu.ifu_broadaddr)->sin_addr.s_addr;
            i++;
        }
    }

    freeifaddrs(ifs);
    return 0;
}

int nis_cntl(objhld_t link, int cmd, ...)
{
    ncb_t *ncb;
    int retval;
    va_list ap;
    void *context;

    ncb = objrefr(link);
    if (!ncb) {
        return -ENOENT;
    }

    retval = 0;

    va_start(ap, cmd);
    switch (cmd) {
        case NI_SETATTR:
            ncb->protocol == IPPROTO_TCP ? tcp_setattr_r(ncb, va_arg(ap, int)) :
                (ncb->protocol == IPPROTO_UDP ? udp_setattr_r(ncb, va_arg(ap, int)) : 0);
            break;
        case NI_GETATTR:
            ncb->protocol == IPPROTO_TCP ? tcp_getattr_r(ncb, &retval) :
                (ncb->protocol == IPPROTO_UDP ? udp_getattr_r(ncb, &retval) : 0);
            break;
        case NI_SETCTX:
            ncb->prcontext = __sync_lock_test_and_set(&ncb->context, va_arg(ap, const void *));
            break;
        case NI_GETCTX:
            ncb->prcontext = __sync_lock_test_and_set(&context, ncb->context);
            *(va_arg(ap, void **) ) = context;
            break;
        case NI_SETTST:
            retval = tcp_settst_r(link, va_arg(ap, const void *));
            break;
        case NI_GETTST:
            retval = tcp_gettst_r(link, va_arg(ap, void *), NULL);
            break;
        default:
            return -EINVAL;
    }
    va_end(ap);

    objdefr(link);
    return retval;
}

int nis_getifmac(char *eth_name, unsigned char *pyhaddr)
{
    struct ifreq ifr;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd > 0) {
        strncpy(ifr.ifr_name, (char *)eth_name, sizeof(ifr.ifr_name) );
        if(ioctl(fd, SIOCGIFHWADDR, &ifr) >= 0) {
            memcpy(pyhaddr, ifr.ifr_hwaddr.sa_data, 6);
        }
        close(fd);
    }
    return posix__makeerror(errno);
}
