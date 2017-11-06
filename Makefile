TARGET=nshost.so.9.4

SRCS=$(wildcard *.c) $(wildcard ../libnsp/com/*.c)
OBJS=$(patsubst %.c,%.o,$(SRCS))

CFLAGS+=-I ../libnsp/icom -fPIC -Wall
LDFLAGS=-shared

ifeq ($(DEBUG_SYMBOLS),TRUE)
	CFLAGS+=-g
else
	CFLAGS+=-O3
endif

all:$(TARGET)

$(TARGET):$(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o:%.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	$(RM) $(OBJS) $(TARGET)

.PHONY:clean all
