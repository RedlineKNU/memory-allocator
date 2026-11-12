UTILS_PATH ?= ../utils

CC = gcc
CPPFLAGS = -I$(UTILS_PATH)
CFLAGS = -fPIC -Wall -Wextra -g -fvisibility=hidden
LDFLAGS = -shared

SRCS = osmem.c blocks.c heap.c $(UTILS_PATH)/printf.c
OBJS = $(SRCS:.c=.o)
TARGET = libosmem.so

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) ${LDFLAGS} -o $@ $^

pack: clean
	-rm -f ../src.zip
	-zip -r ../src.zip *

clean:
	-rm -f ../src.zip
	-rm -f $(TARGET)
	-rm -f $(OBJS)
