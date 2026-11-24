# AGENTS.md

Guide for working on **6809Emulator** — a Motorola 6809 CPU emulator in C
that boots the Dragon 32 home computer to its BASIC `OK` prompt.

## Project structure

```
src/main.c          ncurses front end: interactive terminal screen, batch /
                    trace mode, `--sdl` dispatch
src/sdl.c           SDL2 front end: 256x192 picture scaled x3, sound,
                    Dragon keyboard matrix from SDL keys
src/tapefile.c      cassette tape files as FILE* (input + recorder), shared
src/tapefile.h        by both front ends
src/core/           the shared Dragon core (also lives in 6809Emulator-Metal):
  dragon_cpu.c      6809 CPU, memory map, PIAs, SAM, keyboard matrix, timing,
                    sound generation
  tape.c            cassette deck (CAS/WAV in, WAV+CAS recording)
  vdg.c             MC6847 video: palette, font, vdg_render() picture
  dragon.h          the core's API (what a platform can call)
  tape.h            tape decoding internals
tests/smoke.sh      boot-and-type test (needs your own d32.rom)
Makefile            optional SDL2 detection (HAS_SDL)
README.md           full user documentation
```

## Current state

What works (verified against XRoar / MAME where noted):

- Full documented 6809 instruction set incl. `$10`/`$11` pages, all indexed
  addressing, `DAA`, `MUL`, `SWI`/`SWI2`/`SWI3`, `CWAI`, `SYNC`, with real
  cycle timing matching XRoar's trace.
- Real speed: 17,784 cycles per 50 Hz frame (PAL Dragon); the 50 Hz IRQ
  drives `TIMER`, `SOUND`, `PLAY`.
- PIAs and SAM: both MC6821s, keyboard matrix, sound mux, cassette motor.
- Dragon 32 memory map: 32 KB RAM, 16 KB ROM at `$8000-$BFFF`
  (write-protected), reset vectors mirrored at `$FFF0-$FFFF`.
- Video: 32x16 VDG text screen, semigraphics-4, SG6, graphics modes rendered
  by `vdg_render()` (256x192 palette-index picture). Palette values match
  MAME's measured Dragon palette; 2-colour (RG) modes use black for the off
  colour. Text polarity is dark-on-green (Dragon convention).
- Tapes: `--tape` plays `.cas`/`.wav`, `--record` saves `CSAVE` output.
- Two front ends: ncurses (terminal) and SDL2 (`--sdl`, window + sound +
  live keyboard matrix).
- Batch/trace mode: fixed instruction count, disassembly trace, `--inject`
  typed text, screen dump.

Uncommitted work in progress (not yet in git):

- `src/sdl.c`, `src/tapefile.c/h`: new SDL front end + shared tape file I/O.
- `src/main.c`: `--sdl` dispatch, uses `tapefile`, colour `init_color()` fix
  for the terminal, semigraphics now drawn in their real colour.
- `src/core/vdg.c`: Dragon palette values and RG off-colour (matches MAME).
- `Makefile`/`README.md`: SDL detection and documentation.

## Build and test

```sh
make            # builds ./emulator (SDL2 auto-detected via pkg-config)
make HAS_SDL=0  # build without the SDL front end
make test       # runs tests/smoke.sh with d32.rom (skips if absent)
```

The GitHub Actions build only compiles (no ROM to test with).

Key things to know:

- The ROM is **not** included (still copyrighted). Tests need `d32.rom`
  supplied by the user. `d32.rom` exists locally but must not be committed.
- Build flags: `-O2 -Wall -Wextra`. New code must compile warning-free.
- Code style: tabs for indentation, brace on the same line as `if`/`for`
  (`if (x) {`), C99 declarations, comments only where they explain why.

## Running

```sh
./emulator                 # interactive ncurses when run in a terminal
./emulator --sdl           # SDL2 window with graphics and sound
./emulator -q 4000000 --inject '? 2+3*4\r'   # batch, then screen dump
./emulator --tape game.cas --sdl
```

Keys (both front ends): type to type, F1 reset, F2 turbo, F4 rewind tape,
F10 quit, Esc/F3 BREAK, Home CLEAR, arrows = cursor keys.

## Roadmap

Done (in the working tree, pending commit):

- [x] SDL2 front end (`--sdl`): windowed picture, sound, keyboard.
- [x] Correct Dragon palette + RG 2-colour rendering (matches MAME).
- [x] Terminal colour fix (`init_color` from the Dragon palette).
- [x] Semigraphics colour in the terminal (e.g. `CLS 3` fills solid blue).
- [x] Shared tape-file layer used by both front ends.
- [x] Fixed 50 Hz pacing in the SDL loop (was ~2x speed, audio lag).

Not yet:

- [ ] Commit the in-progress work (sdl.c, tapefile, vdg palette, main.c).
- [ ] Graphics modes and sound in the *terminal* (SDL shows both now).
- [ ] Joysticks.
- [ ] Disk drives / DragonDOS.
- [ ] Dragon 64 and Tandy CoCo ROMs.
- [ ] Sync the shared core changes back to 6809Emulator-Metal
      (tools/sync-core.sh).

## Gotchas

- The SDL loop must pace each frame from its *start* and update the
  timestamp after `SDL_Delay`, or the emulator runs ~2x real speed and the
  audio queue backs up (~1 s of lag per second).
- `dragon_set_matrix()` is ignored while typed keys (`dragon_type*`) are
  pending — the SDL front end uses the matrix, the ncurses one uses typed
  keys, and `--inject` uses typed keys.
- Video polarity: normal Dragon text is dark-on-green (bit 6 set = normal,
  bit 6 clear = inverse). Spaces in VRAM are `$60`, not `$20`.
- The core is shared with 6809Emulator-Metal; keep `src/core/` changes
  portable (plain C, no ncurses/SDL).
