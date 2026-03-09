CC      ?= $(CROSS_COMPILE)gcc
CFLAGS  ?= -Wall -Werror -g
TARGET  = aesdsocket

.PHONY: all default clean

default: all

all: $(TARGET)

$(TARGET): aesdsocket.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lpthread

aesdsocket.o: aesdsocket.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) *.o
