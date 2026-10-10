
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L $(shell pkg-config --cflags freetype2 xft)
LDFLAGS = -lX11 -lXpm -lXrender $(shell pkg-config --libs xft)
TARGET = terminator
SOURCE = src/main.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
