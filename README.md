# 6809Emulator

A Motorola 6809 CPU emulator in C that boots the **Dragon 32** home computer
(1982) to its BASIC `OK` prompt, in your terminal.

```
+--------------------------------+
|(C) 1982 DRAGON DATA LTD        |
|16K BASIC INTERPRETER 1.0       |
|(C) 1982 BY MICROSOFT           |
|                                |
|OK                              |
|? 2+3*4                         |
| 14                             |
|OK                              |
+--------------------------------+
 PC:BBC8  A:00 B:01 X:03D3 Y:824E S:7F2A
 CC:[..H.....]  DP:00
 [F10]:Exit [F1]:Reset [F2]:Turbo [ESC/F3]:Break
```

It is a hobby project with no dependencies beyond ncurses, written to learn
how the 6809 and the Dragon work. It started in 2021 as a way to step
through the Dragon ROM one instruction at a time.

The emulation core (`src/core/`) is shared with
[6809Emulator-Metal](https://github.com/dylan7474/6809Emulator-Metal), the
same Dragon running bare-metal on a Raspberry Pi with HDMI, sound and a tape
deck. Fixes are made there and copied here with its `tools/sync-core.sh`.

## What works

- **6809 CPU**: the full documented instruction set, including page 2 and
  page 3 (`$10`/`$11`) opcodes, every indexed addressing mode, condition codes,
  `DAA`, `MUL`, `SWI`/`SWI2`/`SWI3`, `CWAI` and `SYNC`, with real cycle
  timing (every instruction's cycle count agrees with XRoar's trace).
- **Real speed**: 17,784 cycles per 50 Hz frame, as a PAL Dragon; the
  50 Hz interrupt works, so `TIMER`, `SOUND` and `PLAY` timing are right.
- **PIAs and SAM**: both MC6821s (keyboard, interrupts, sound mux, cassette
  motor) and the SAM's display address.
- **Dragon 32 memory map**: 32 KB RAM, the 16 KB ROM at `$8000-$BFFF`
  (write-protected), reset vectors mirrored at `$FFF0-$FFFF`.
- **Text screen**: the 32x16 VDG text screen (where the SAM points; `$0400`
  for BASIC), drawn with ncurses (inverse video shown as inverted colours,
  semigraphics as `#`). Graphics modes run but a terminal can't show them.
- **Tapes**: `--tape FILE.cas` or `.wav` plays into the cassette port in real
  time for `CLOAD`/`CLOADM`; `--record PREFIX` saves `CSAVE` output as
  `PREFIX.wav` + `PREFIX.cas`. Both load in XRoar and vice versa.
- **Keyboard**: the PIA keyboard matrix, so BASIC reads keys as it would on
  the real machine, including shifted symbols, the cursor keys, CLEAR and
  BREAK.
- **Batch / trace mode**: run a fixed number of instructions, print a
  disassembly trace, type text in automatically and dump the screen at the
  end. Handy for testing and for comparing with MAME.

## Not yet

- Showing graphics modes and playing sound in the terminal (both work in
  6809Emulator-Metal, which shares this core).
- Joysticks, disks (DragonDOS), the Dragon 64 and Tandy CoCo ROMs.

## You need a ROM

The Dragon 32 ROM (Microsoft BASIC) is still copyrighted, so it is **not**
included. You need your own 16 KB image (16,384 bytes), for example one
read from your own machine. By default the emulator looks for `d32.rom` in
the current folder.

## Build

Linux or macOS, with a C compiler and ncurses:

```sh
# Debian/Ubuntu: sudo apt install build-essential libncurses-dev
# Arch:          sudo pacman -S base-devel ncurses
make
```

This builds `./emulator`.

## Run

**Interactive** (full screen, type straight into BASIC):

```sh
./emulator            # starts interactive when run in a terminal
./emulator -i other.rom
```

| Key | Does |
| --- | --- |
| Letters, numbers, symbols | type as normal (the Dragon only has capitals) |
| Enter | ENTER |
| Backspace / Left arrow | cursor left (delete) |
| Arrow keys | Dragon cursor keys |
| Esc or F3 | BREAK |
| Home or Ctrl+L | CLEAR |
| F1 | reset (BASIC warm start; your program is kept) |
| F2 | turbo on/off (run as fast as the host allows) |
| F4 | rewind the tape |
| F10, Ctrl+Q or Ctrl+C | quit |

**Batch / trace** (no screen; good for testing):

```sh
./emulator 200                 # trace the first 200 instructions
./emulator -q 3000000          # run quietly, then print registers + screen
./emulator -q 4000000 --inject '? 2+3*4\r'
```

| Option | Meaning |
| --- | --- |
| `N` (a number) | how many instructions to run (default 150) |
| `-q`, `--quiet` | no per-instruction trace |
| `-i`, `--interactive` | force the full-screen mode |
| `--inject TEXT` | type TEXT once BASIC shows `OK` (run at least ~3,000,000 instructions; typing goes at Dragon speed) |
| `--tape FILE` | insert a `.cas` or `.wav` tape |
| `--record PREFIX` | save what the Dragon writes to tape as `PREFIX.wav` + `PREFIX.cas` |
| any other word | the ROM file to load |

Escapes inside `--inject` text: `\r` or `\n` = ENTER, `\b` = BREAK,
`\c` = CLEAR.

## Test

```sh
./tests/smoke.sh path/to/d32.rom
```

This boots BASIC, types a sum and a `GOTO` loop, presses BREAK and checks the
screen shows ` 14` and `BREAK IN 10`. Without a ROM it skips. The GitHub
Actions build only compiles the code, because there is no ROM to test with.

To check the CPU against a reference, `mame dragon32 -debug` steps through
the same ROM, and `dump memory.txt,FF00,10` in its debugger saves memory to
compare.

## Layout

```
src/main.c          the terminal: ncurses screen and keys, batch / trace mode, tape files
src/core/           the shared Dragon core (from 6809Emulator-Metal):
  dragon_cpu.c      6809 CPU, memory map, PIAs, SAM, keyboard matrix, timing, sound
  tape.c            cassette deck (CAS/WAV in, WAV+CAS recording)
  vdg.c             MC6847 video
  dragon.h          the core's API
tests/smoke.sh      boot-and-type test (needs your ROM)
Makefile
```

## Further reading

- *Language of the Dragon: 6809 Assembler* by Mike James (Melbourne House,
  1983): the book this project learned from.
- Motorola *MC6809 Programming Manual* and datasheet.
- The [Dragon Archive](https://archive.worldofdragon.org/) for hardware notes.

## Licence

GPL-2.0-or-later. See [LICENSE](LICENSE).
