TARGET=nshost.so.9.5.1
build=automatic
arch=x86_64

SRCS=$(wildcard *.c) $(wildcard ../libnsp/com/*.c)
OBJS=$(patsubst %.c,%.o,$(SRCS))

CFLAGS+=-I ../libnsp/icom -fPIC -Wall
LDFLAGS=-shared

ifeq ($(build),debug)
	CFLAGS+=-g
else
	CFLAGS+=-O2
endif

ifeq ($(arch),arm)
	CC=arm-linux-gnueabihf-gcc
else
	ifeq ($(arch), i686)
		CC=gcc
		CFLAGS+=-m32
		LDFLAGS+=-m32
	else
		CC=gcc
	endif
endif

all:$(TARGET)

$(TARGET):$(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o:%.c
	$(CC) -c $< $(CFLAGS)  -o $@
clean:
	$(RM) $(OBJS) nshost.so*

.PHONY:clean all
