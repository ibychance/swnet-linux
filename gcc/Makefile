TARGET=nshost.so.$(VERSION)

SRCS=$(wildcard *.c)
OBJS=$(patsubst %.c,%.o,$(SRCS))

CFLAGS+=-I ../../libnsp/icom -fPIC -Wall
LDFLAGS=-L ../ -lnsp -lpthread -lrt -ldl -shared

ifeq ($(DEBUG_SYMBOLS),TRUE)
	CFLAGS+=-g
else
	CFLAGS+=-o3
endif

all:$(TARGET)

$(TARGET):$(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^
	mv $(TARGET) ../	

%.o:%.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	$(RM) $(OBJS) $(TARGET) ../$(TARGET)

.PHONY:clean all

include ../Makefile.rule
