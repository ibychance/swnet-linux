TARGET=nshost.echo

build=release
arch=Ia64

RM=rm

SRCS+=$(wildcard ./*.c)
CFLAGS+=-I ./ -I ../../libnsp/icom/ -Wall -std=gnu99 -D_GNU_SOURCE

ifeq ($(build),debug)
	CFLAGS+=-g3 -fstack-protector-all -fstack-check=specific -fno-omit-frame-pointer -fno-optimize-sibling-calls
else
	CFLAGS+=-O2
endif

ifeq ($(ecr),set)
	CFLAGS+=-D_SET_ECR=1
endif

ifneq ($(nshost),yes)
	SRCS+=$(wildcard ../../libnsp/com/*.c)
	SRCS+=$(wildcard ../*.c)
endif

OBJS=$(patsubst %.c,%.o,$(SRCS))
COMPILER=cc
CC=$(COMPILER)
OBJCOPY=objcopy

LD_ARCH_PATH=/usr/local/lib64/
ifeq ($(arch),arm)
    CC=arm-linux-gnueabihf-$(COMPILER)
    LD_ARCH_PATH:=/usr/local/lib/
	CFLAGS+=-mfloat-abi=hard -mfpu=neon
endif

ifeq ($(arch),arm64)
    CC=aarch64-linux-gnu-$(COMPILER)
endif

LDFLAGS=
ifeq ($(nshost),yes)
	LDFLAGS+=$(LD_ARCH_PATH)nshost.so -Wl,-rpath=$(LD_ARCH_PATH)
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
