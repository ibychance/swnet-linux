TARGET=arp

build=release
arch=x86_64

RM=rm

SRCS=arp.c
OBJS=$(patsubst %.c,%.o,$(SRCS))

CFLAGS+=-I ./ -I ./libnsp/icom/
CFLAGS+=-Wall -std=gnu99

ifeq ($(build),debug)
	CFLAGS+=-g
else
	CFLAGS+=-O2
endif

ifeq ($(arch),arm)
        CC=arm-linux-gnueabihf-gcc
        CFLAGS+=-mfloat-abi=hard -mfpu=neon
else
        CC=gcc
endif

LDFLAGS:=/usr/local/lib64/nshost.so -Wl,-rpath=/usr/local/lib64/
LDFLAGS+=-lrt -lm -lpthread -ldl

all:$(TARGET)

$(TARGET):$(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o:%.c
	$(CC) -c $< $(CFLAGS) -o $@

clean:
	$(RM) -f $(OBJS) $(TARGET)

.PHONY:clean all install
