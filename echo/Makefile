TARGET=nshost.echo

build=release
arch=Ia64

RM=rm

SRCS+=$(wildcard ./*.c)
SRCS+=$(wildcard ../*.c)
SRCS+=$(wildcard ../../libnsp/com/*.c)

OBJS=$(patsubst %.c,%.o,$(SRCS))

CFLAGS+=-I ./ -I ../ -I ../../libnsp/icom/

CFLAGS+=-Wall -std=gnu99 -D_GNU_SOURCE

ifeq ($(build),debug)
	CFLAGS+=-g3 -fstack-protector-all
else
	CFLAGS+=-O2
endif

COMPILER=gcc
CC=$(COMPILER)
OBJCOPY=objcopy

ifeq ($(arch),arm)
    CC=arm-linux-gnueabihf-$(COMPILER)
	CFLAGS+=-mfloat-abi=hard -mfpu=neon
endif

ifeq ($(arch),arm64)
    CC=aarch64-linux-gnu-$(COMPILER)
endif

LDFLAGS+=-ldl -lpthread -lrt -lcrypt

all:$(TARGET)

$(TARGET):$(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o:%.c
	$(CC) -c $< $(CFLAGS) -o $@

clean:
	$(RM) -f $(OBJS) $(TARGET)


.PHONY:clean all install
