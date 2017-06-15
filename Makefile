TARGET=nshost.so.8.1.1

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
	mv $(TARGET) ../	

%.o:%.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	$(RM) $(OBJS) $(TARGET) ../$(TARGET)

.PHONY:clean all
