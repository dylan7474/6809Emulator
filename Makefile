CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  := -lncurses

# The Dragon core is shared with 6809Emulator-Metal (the bare-metal Pi
# version); it is copied here by that project's tools/sync-core.sh.
CORE    := src/core/dragon_cpu.c src/core/vdg.c src/core/tape.c
HEADERS := src/core/dragon.h src/core/tape.h

all: emulator

emulator: src/main.c $(CORE) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ src/main.c $(CORE) $(LDLIBS)

test: emulator
	./tests/smoke.sh $(ROM)

clean:
	rm -f emulator

.PHONY: all clean test
