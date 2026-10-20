CC := gcc
CFLAGS := -Wall -Wextra -Wpedantic -std=c11 \
          -D_XOPEN_SOURCE=700 \
          -D_POSIX_C_SOURCE=200809L \
          $(shell pkg-config --cflags freetype2 xft)
LDLIBS := -pthread -lX11 -lXpm -lXrender $(shell pkg-config --libs xft)
TARGET := terminator
SOURCE := src/main.c
OBJECTS := $(SOURCE:.c=.o)

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJECTS)
