TARGET=nshost.so.9.5
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
	CC=gcc
endif

all:$(TARGET)

$(TARGET):$(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o:%.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	$(RM) $(OBJS) $(TARGET)

.PHONY:clean all
