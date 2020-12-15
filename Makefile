PROGRAM=nshost.so
VERSION=.9.9.1
DETACHED=.detached
DEBUGINFO=.debuginfo

TARGET=$(PROGRAM)$(VERSION)
build=release
arch=IA64
SRC_EXT=c
SYS_WIDTH=$(shell getconf LONG_BIT)

SRCS=./fifo.c ./io.c ./mxx.c ./ncb.c ./tcp.c ./tcpal.c ./pipe.c \
		./tcpio.c ./udp.c ./udpio.c ./arp.c ./arpio.c ./wpool.c

SRCS+=../libnsp/com/avltree.c ../libnsp/com/logger.c ../libnsp/com/posix_ifos.c ../libnsp/com/posix_string.c \
		../libnsp/com/posix_time.c ../libnsp/com/hash.c ../libnsp/com/object.c ../libnsp/com/posix_naos.c \
		../libnsp/com/posix_thread.c ../libnsp/com/posix_wait.c ../libnsp/cfifo.c

#SRCS=$(foreach dir, $(DIRS), $(wildcard $(dir)/*.$(SRC_EXT)))
#OBJS=$(addprefix $(OBJS_DIR)/,$(patsubst %.$(SRC_EXT),%.o,$(notdir $(SRCS))))

INC_DIR=-I ../libnsp/icom/
CFLAGS+=$(INC_DIR) -fPIC -Wall -std=c89 -ansi -D_GNU_SOURCE -fvisibility=hidden

MIN_GCC_VERSION = "4.9"
GCC_VERSION := "`$(CC) -dumpversion`"
IS_GCC_ABOVE_MIN_VERSION := $(shell expr "$(GCC_VERSION)" ">=" "$(MIN_GCC_VERSION)")
ifeq "$(IS_GCC_ABOVE_MIN_VERSION)" "1"
    CFLAGS+=-fstack-protector-all -fstack-check=specific
else
    CFLAGS+=-fstack-protector
endif

LDFLAGS=-shared -lcrypt -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack

OBJCOPY=objcopy
INSTALL_DIR=/usr/local/lib64/

ifeq ($(arch), i686)
	CFLAGS+=-m32
	LDFLAGS+=-m32
	INSTALL_DIR=/usr/local/lib/
endif

ifeq ($(arch), $(filter $(arch),arm arm32))
	CC=arm-linux-gnueabihf-gcc
	OBJCOPY=arm-linux-gnueabihf-objcopy
	CFLAGS+=-mfloat-abi=hard -mfpu=neon
	INSTALL_DIR=/usr/local/lib/
endif

ifeq ($(arch), $(filter $(arch),arm64 aarch64))
	CC=aarch64-linux-gnu-gcc
	OBJCOPY=aarch64-linux-gnu-objcopy
	INSTALL_DIR=/usr/local/lib/aarch64-linux-gnu/
endif

ifeq ($(build),debug)
	CFLAGS+=-g3 -fno-omit-frame-pointer -fno-optimize-sibling-calls
else
	CFLAGS+=-O2
endif

# define the middle directory for build
BUILD_DIR=tmp
OBJS_DIR=$(BUILD_DIR)/objs
DEPS_DIR=$(BUILD_DIR)/deps
# determine the complier target directorys
DIRS=./ ../libnsp/com/
# add all $(DIRS) into vpath variable
VPATH = $(DIRS)
# construct the object output files
OBJS=$(addprefix $(OBJS_DIR)/,$(patsubst %.$(SRC_EXT),%.o,$(notdir $(SRCS))))
DEPS=$(addprefix $(DEPS_DIR)/,$(patsubst %.$(SRC_EXT),%.d,$(notdir $(SRCS))))

$(TARGET):$(PROGRAM)
	cp -f $(PROGRAM) $(TARGET)

$(PROGRAM):$(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(OBJS_DIR)/%.o:%.$(SRC_EXT)
	@if [ ! -d $(OBJS_DIR) ]; then mkdir -p $(OBJS_DIR); fi;
	$(CC) -c $< $(CFLAGS) -o $@

$(DEPS_DIR)/%.d:%.$(SRC_EXT)
	@if [ ! -d $(DEPS_DIR) ]; then mkdir -p $(DEPS_DIR); fi;
	set -e; rm -f $@;\
	$(CC) -MM $(CFLAGS) $< > $@.$$$$;\
	sed 's,\($*\)\.o[ :]*,$(OBJS_DIR)/\1.o $@ : ,g' < $@.$$$$ > $@;\
	rm -f $@.$$$$

-include $(DEPS)

.PHONY:clean all install detach

all:
	$(TARGET)

clean:
	rm -rf $(OBJS_DIR) $(DEPS_DIR)
	rm -f $(PROGRAM)*
	rm -f ./*.o

install:
	install -m644 $(TARGET) $(INSTALL_DIR)
	ln -sf $(INSTALL_DIR)$(TARGET) $(INSTALL_DIR)$(PROGRAM)

detach:
	$(OBJCOPY) --only-keep-debug $(PROGRAM) $(PROGRAM)$(DEBUGINFO)
	$(OBJCOPY) --strip-unneeded $(PROGRAM) $(PROGRAM)$(DETACHED)
