PROGRAM=nshost.so
VERSION=10.0.0

TARGET=$(PROGRAM).$(VERSION)
build=release
arch=ia64
SRC_EXT=c
SYS_WIDTH=$(shell getconf LONG_BIT)

SRCS=./fifo.c ./io.c ./mxx.c ./ncb.c ./tcp.c ./tcpal.c \
		./tcpio.c ./udp.c ./udpio.c ./arp.c ./arpio.c ./wpool.c

SRCS+=../libnsp/com/avltree.c ../libnsp/com/logger.c ../libnsp/com/posix_ifos.c ../libnsp/com/posix_string.c \
		../libnsp/com/posix_time.c ../libnsp/com/hash.c ../libnsp/com/object.c ../libnsp/com/posix_naos.c \
		../libnsp/com/posix_thread.c ../libnsp/com/posix_wait.c

#SRCS=$(foreach dir, $(DIRS), $(wildcard $(dir)/*.$(SRC_EXT)))
#OBJS=$(addprefix $(OBJS_DIR)/,$(patsubst %.$(SRC_EXT),%.o,$(notdir $(SRCS))))

INC_DIR=-I ../libnsp/icom/
CFLAGS+=$(INC_DIR) -fPIC -Wall -std=c89 -ansi -D_GNU_SOURCE
LDFLAGS=-shared -lcrypt

MIN_GCC_VERSION = "4.9"
GCC_VERSION := "`$(CC) -dumpversion`"
IS_GCC_ABOVE_MIN_VERSION := $(shell expr "$(GCC_VERSION)" ">=" "$(MIN_GCC_VERSION)")
ifeq ($(build),debug)
	ifeq "$(IS_GCC_ABOVE_MIN_VERSION)" "1"
	    CFLAGS += -fstack-protector-strong
	else
	    CFLAGS += -fstack-protector
	endif
	CFLAGS+=-g3
else
	CFLAGS+=-O2
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

# define the build middle directory
BUILD_DIR=tmp
OBJS_DIR=$(BUILD_DIR)/objs
DEPS_DIR=$(BUILD_DIR)/deps
# determine the complier target directorys
DIRS=./ ../libnsp/com/
# add all $(DIRS) into vpath variable
VPATH = $(DIRS)
# construct the object output files
OBJS=$(addprefix $(OBJS_DIR)/,$(patsubst %.$(SRC_EXT),%.o,$(notdir $(SRCS))))
DEPS=$(addprefix $(DEPS_DIR)/, $(patsubst %.$(SRC_EXT),%.d,$(notdir $(SRCS))))

$(TARGET):$(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

#%.o:%.$(SRC_EXT)
#	$(CC) -c $< $(CFLAGS)  -o $@

$(OBJS_DIR)/%.o:%.$(SRC_EXT)
	@if [ ! -d $(OBJS_DIR) ]; then mkdir -p $(OBJS_DIR); fi;
	$(CC) -c $< $(CFLAGS)  -o $@

$(DEPS_DIR)/%.d:%.$(SRC_EXT)
	@if [ ! -d $(DEPS_DIR) ]; then mkdir -p $(DEPS_DIR); fi;
	set -e; rm -f $@;\
	$(CC) -MM $(CFLAGS) $< > $@.$$$$;\
	sed 's,\($*\)\.o[ :]*,$(OBJS_DIR)/\1.o $@ : ,g' < $@.$$$$ > $@;\
	rm -f $@.$$$$

-include $(DEPS)

.PHONY:clean all install

all:
	$(TARGET)

clean:
	rm -rf $(OBJS_DIR) $(DEPS_DIR)
	rm -f $(PROGRAM)*

install:
	install -m644 $(TARGET) $(INSTALL_DIR)
	ln -sf $(INSTALL_DIR)$(TARGET) $(INSTALL_DIR)$(PROGRAM)
