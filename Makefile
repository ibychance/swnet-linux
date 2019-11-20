PROGRAM=nshost.so
VERSION=9.8.3

TARGET=$(PROGRAM).$(VERSION)
build=release
arch=ia64
SRC_EXT=c
SYS_WIDTH=$(shell getconf LONG_BIT)

SRCS=./fifo.c \
		./io.c \
		./mxx.c \
		./ncb.c \
		./tcp.c \
		./tcpal.c \
		./tcpio.c \
		./udp.c \
		./udpio.c \
		./arp.c \
		./arpio.c \
		./wpool.c

SRCS+=../libnsp/com/avltree.c \
		../libnsp/com/logger.c \
		../libnsp/com/posix_ifos.c \
		../libnsp/com/posix_string.c \
		../libnsp/com/posix_time.c \
		../libnsp/com/hash.c \
		../libnsp/com/object.c \
		../libnsp/com/posix_naos.c \
		../libnsp/com/posix_thread.c \
		../libnsp/com/posix_wait.c

OBJS=$(patsubst %.$(SRC_EXT),%.o,$(SRCS))

CFLAGS+=-I ../libnsp/icom -fPIC -Wall -std=c89 -ansi -D_GNU_SOURCE
LDFLAGS=-shared -lcrypt

ifeq ($(build),debug)
	CFLAGS+=-g3
else
	CFLAGS+=-O2
endif

MIN_GCC_VERSION = "4.9"
GCC_VERSION := "`$(CC) -dumpversion`"
IS_GCC_ABOVE_MIN_VERSION := $(shell expr "$(GCC_VERSION)" ">=" "$(MIN_GCC_VERSION)")
ifeq "$(IS_GCC_ABOVE_MIN_VERSION)" "1"
    CFLAGS += -fstack-protector-strong
else
    CFLAGS += -fstack-protector
endif

INSTALL_DIR=/usr/local/lib64/

ifeq ($(arch),arm)
	CC=arm-linux-gnueabihf-gcc
	CFLAGS+=-mfloat-abi=hard -mfpu=neon
	INSTALL_DIR=/usr/local/lib/
endif

ifeq ($(arch), i686)
	CFLAGS+=-m32
	LDFLAGS+=-m32
	INSTALL_DIR=/usr/local/lib/
endif

ifeq ($(arch), arm64)
	CC=aarch64-linux-gnu-gcc
	INSTALL_DIR=/usr/local/lib/aarch64-linux-gnu/
endif


all:$(TARGET)

$(TARGET):$(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o:%.$(SRC_EXT)
	$(CC) -c $< $(CFLAGS)  -o $@

clean:
	$(RM) $(OBJS) $(PROGRAM)*

install:
	install -m644 $(TARGET) $(INSTALL_DIR)
	ln -sf $(INSTALL_DIR)$(TARGET) $(INSTALL_DIR)$(PROGRAM)

.PHONY:clean all install
