CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  := -lncurses

all: emulator

emulator: src/emulator.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f emulator

.PHONY: all clean
