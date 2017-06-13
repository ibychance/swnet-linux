/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <stddef.h>
#include <ctype.h>

#include "nis.h"
#include "ncb.h"
#include "object.h"

int nis_setctx(HLNK lnk, const void * user_context, int user_context_size) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;

    ncb = objrefr(hld);
    if (!ncb) return -1;

    ncb->context_size = 0;

    /*ָ����ָ���0���ȣ�������յ�ǰ���û�������*/
    if (!user_context || 0 == user_context_size) {
        if (ncb->context && ncb->context_size > 0) {
            free(ncb->context);
            ncb->context = NULL;
        }
        objdefr(hld);
        return 0;
    }

    /*ȷ���Ƿ������û������ĵĳ���*/
    if (user_context_size != ncb->context_size && ncb->context) {
        free(ncb->context);
        ncb->context = NULL;
    }
    if (!ncb->context) {
        ncb->context = (char *) malloc(user_context_size);
        if (!ncb->context) {
            objdefr(hld);
            return -1;
        }
    }
    ncb->context_size = user_context_size;
    memcpy(ncb->context, user_context, ncb->context_size);
    objdefr(hld);
    return 0;
}

int nis_getctx(HLNK lnk, void * user_context, int *user_context_size/*OPT*/) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;

    if (!user_context) return -1;

    ncb = objrefr(hld);
    if (!ncb) return -1;

    if (!ncb->context || 0 == ncb->context_size) {
        objdefr(hld);
        return -1;
    }

    if (user_context_size) *user_context_size = ncb->context_size;

    memcpy(user_context, ncb->context, ncb->context_size);
    objdefr(hld);
    return 0;
}

void *nis_refctx(HLNK lnk, int *user_context_size) {
    void *ctxdata;
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;

    ncb = objrefr(hld);
    if (!ncb) return NULL;

    if (!ncb->context || 0 == ncb->context_size) {
        objdefr(hld);
        return NULL;
    }

    ctxdata = ncb->context;
    if (user_context_size) {
        *user_context_size = ncb->context_size;
    }

    objdefr(hld);
    return ctxdata;
}

int nis_ctxsize(HLNK lnk) {
    ncb_t *ncb;
    objhld_t hld = (objhld_t) lnk;
    int cb;

    ncb = objrefr(hld);
    if (!ncb) return -1;

    cb = ncb->context_size;

    objdefr(hld);
    return cb;
}

int nis_getver(swnet_version_t *version) {
    if (!version) return -1;

    version->procedure_ = 0;
    version->main_ = 1;
    version->sub_ = 1;
    version->leaf_ = 13;
    return 0;
}

int nis_gethost(const char *name, uint32_t *ipv4) {
    struct hostent *remote;
    struct in_addr addr;

    if (!name || !ipv4) return -1;

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

    if (!remote) return -1;

    /* Ŀǰ��֧�� IPv4 */
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