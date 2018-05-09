TARGET=nshost.so.9.6.3
build=automatic
arch=x86_64
INSTALL_DIR=
SRC_EXT=c
SYS_WIDTH=$(shell getconf LONG_BIT)

SRCS=$(wildcard *.$(SRC_EXT)) $(wildcard ../libnsp/com/*.$(SRC_EXT))
OBJS=$(patsubst %.$(SRC_EXT),%.o,$(SRCS))

CFLAGS+=-I ../libnsp/icom -fPIC -Wall -std=gnu99
LDFLAGS=-shared

ifeq ($(build),debug)
	CFLAGS+=-g
else
	CFLAGS+=-O2
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
