TARGET=nshost.echo

build=release
arch=Ia64

RM=rm

SRCS+=$(wildcard ./*.c)
SRCS+=$(wildcard ../*.c)
SRCS+=$(wildcard ../../libnsp/com/*.c)

CFLAGS+=-I ./ -I ../../libnsp/icom/ -Wall -std=gnu99 -D_GNU_SOURCE

ifeq ($(build),debug)
	CFLAGS+=-g3 -fstack-protector-all -fstack-check=specific -fno-omit-frame-pointer -fno-optimize-sibling-calls
else
	CFLAGS+=-O2
endif

ifeq ($(ecr),set)
	CFLAGS+=-D_SET_ECR=1
endif

OBJS=$(patsubst %.c,%.o,$(SRCS))
COMPILER=cc
CC=$(COMPILER)
OBJCOPY=objcopy

ifeq ($(arch),arm)
    CC=arm-linux-gnueabihf-gcc
	CFLAGS+=-mfloat-abi=hard -mfpu=neon
endif

ifeq ($(arch), $(filter $(arch),arm64 aarch64))
    CC=aarch64-linux-gnu-gcc
endif

LDFLAGS:=-ldl -lpthread -lrt -lcrypt -fPIE

all:$(TARGET)

$(TARGET):$(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o:%.c
	$(CC) -c $< $(CFLAGS) -o $@

clean:
	$(RM) -f $(OBJS) $(TARGET)


.PHONY:clean all install
