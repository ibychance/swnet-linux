#ifndef PIPE_H_20170118
#define PIPE_H_20170118

#include "ncb.h"

extern int pipe_create(int protocol);
extern
int pipe_write_message(ncb_t *ncb, const unsigned char *data, int cb);

#endif
