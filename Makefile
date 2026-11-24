CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  := -lncurses

# The Dragon core is shared with 6809Emulator-Metal (the bare-metal Pi
# version); it is copied here by that project's tools/sync-core.sh.
CORE    := src/core/dragon_cpu.c src/core/vdg.c src/core/tape.c
HEADERS := src/core/dragon.h src/core/tape.h

# SDL2 is optional (for the --sdl front end).  `make HAS_SDL=0` disables it.
HAS_SDL ?= $(shell pkg-config --exists sdl2 && echo 1 || echo 0)
ifeq ($(HAS_SDL),1)
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)
SDL_SRC    := src/sdl.c
else
SDL_CFLAGS :=
SDL_SRC    :=
endif

CFLAGS += -DHAS_SDL=$(HAS_SDL) $(SDL_CFLAGS)

all: emulator

emulator: src/main.c src/tapefile.c $(SDL_SRC) $(CORE) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ src/main.c src/tapefile.c $(SDL_SRC) $(CORE) $(LDLIBS) $(SDL_LIBS)

test: emulator
	./tests/smoke.sh $(ROM)

clean:
	rm -f emulator

.PHONY: all clean test
