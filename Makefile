TARGET=nshost.so.9.6.4
build=automatic
arch=x86_64
INSTALL_DIR=
SRC_EXT=c
SYS_WIDTH=$(shell getconf LONG_BIT)

SRCS=./fque.c \
		./io.c \
		./mxx.c \
		./ncb.c \
		./tcp.c \
		./tcpal.c \
		./tcpio.c \
		./udp.c \
		./udpio.c \
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

CFLAGS+=-I ../libnsp/icom -fPIC -Wall -Werror -std=gnu99
LDFLAGS=-shared

ifeq ($(build),debug)
	CFLAGS+=-g
else
	ifeq ($(build),gdb)
		CFLAGS+=-ggdb3
	else
		CFLAGS+=-O2
	endif
endif

ifeq ($(arch),arm)
	CC=arm-linux-gnueabihf-gcc
	CFLAGS+=-mfloat-abi=hard -mfpu=neon
	INSTALL_DIR+=/usr/local/lib/
else
	ifeq ($(arch), i686)
		CC=gcc
		CFLAGS+=-m32
		LDFLAGS+=-m32
		INSTALL_DIR+=/usr/local/lib/
	else
		CC=gcc
		INSTALL_DIR+=/usr/local/lib64/
	endif
endif

all:$(TARGET)

$(TARGET):$(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o:%.$(SRC_EXT)
	$(CC) -c $< $(CFLAGS)  -o $@

clean:
	$(RM) $(OBJS) nshost.so*

install:
	cp -f $(TARGET) $(INSTALL_DIR)
	ln -sf $(INSTALL_DIR)$(TARGET) $(INSTALL_DIR)nshost.so

.PHONY:clean all install
