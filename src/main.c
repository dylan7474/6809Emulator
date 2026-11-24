/*
 * main.c - 6809Emulator: the Dragon 32 in a terminal (ncurses), and a
 * batch / trace mode for testing
 *
 * Copyright (C) 2021-2026 Dylan Jones
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The emulation itself is the shared core in src/core/ (the same files as
 * 6809Emulator-Metal, the bare-metal Raspberry Pi version): the 6809 CPU
 * with real cycle timing, the PIAs and 50 Hz interrupt, the SAM, the VDG
 * and the cassette deck.  This file is only the terminal around it.
 */
#include "core/dragon.h"
#include "tapefile.h"
#include <ctype.h>
#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef HAS_SDL
#define HAS_SDL 0
#endif

#if HAS_SDL
int sdl_main(int argc, char **argv);
#endif

static bool ncurses_active;

static void cleanup_and_exit(int sig)
{
	(void)sig;
	if (ncurses_active)
	{
		endwin();
		ncurses_active = false;
	}
	exit(0);
}

// ---- Screen --------------------------------------------------------------------

static unsigned video_base(void)
{
	return ((dragon_sam() >> 3) & 0x7F) * 512u;
}

static void dump_screen(void)
{
	char screen[16 * 33 + 1];
	vdg_text(dragon_memory(), screen);
	printf("\n--- DRAGON 32 TEXT SCREEN ($%04X) ---\n", video_base());
	printf("+--------------------------------+\n");
	for (char *line = screen; *line; line += 33)
	{
		printf("|%.32s|\n", line);
	}
	printf("+--------------------------------+\n");
}

static void print_regs(void)
{
	DragonRegs r;
	dragon_regs(&r);
	printf("A: %02X, B: %02X, D: %04X, X: %04X, Y: %04X, U: %04X, S: %04X, DP: %02X\n",
	       r.a, r.b, (r.a << 8) | r.b, r.x, r.y, r.u, r.s, r.dp);
	printf("CC: %02X [%c%c%c%c%c%c%c%c]\n", r.cc,
	       (r.cc & 0x80) ? 'E' : '.', (r.cc & 0x40) ? 'F' : '.', (r.cc & 0x20) ? 'H' : '.',
	       (r.cc & 0x10) ? 'I' : '.', (r.cc & 0x08) ? 'N' : '.', (r.cc & 0x04) ? 'Z' : '.',
	       (r.cc & 0x02) ? 'V' : '.', (r.cc & 0x01) ? 'C' : '.');
}

// The eight 6847 semigraphics colours, as the nearest ncurses colour
// (orange has no exact ncurses colour, so it shares red's slot).
static int sg_colour(unsigned i)
{
	static const int map[8] = {
		COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE, COLOR_RED,
		COLOR_WHITE, COLOR_CYAN, COLOR_MAGENTA, COLOR_RED
	};
	return map[i & 7];
}

static void draw_screen(bool turbo)
{
	const int top = 1, left = 2;
	const uint8_t *mem = dragon_memory();
	unsigned base = video_base();

	attron(COLOR_PAIR(3));
	mvprintw(top - 1, left - 1, "+--------------------------------+");
	for (int r = 0; r < 16; r++)
	{
		mvaddch(top + r, left - 1, '|');
		mvaddch(top + r, left + 32, '|');
	}
	mvprintw(top + 16, left - 1, "+--------------------------------+");
	attroff(COLOR_PAIR(3));

	if (mem[0xFF22] & 0x80)
	{
		// A graphics mode (PMODE etc): a terminal can't show it
		for (int r = 0; r < 16; r++)
		{
			mvprintw(top + r, left, "%-32s", r == 7 ? "    (graphics mode: not shown" : r == 8 ? "     in a terminal)" : "");
		}
	}
	else
	{
		for (int r = 0; r < 16; r++)
		{
			for (int c = 0; c < 32; c++)
			{
				uint8_t ch = mem[(base + r * 32 + c) & 0xFFFF];
				char out;
				int pair;
				if (ch & 0x80)
				{
					// Semigraphics-4: bits 6-4 are the colour, bits 3-0
					// which quarters of the cell are lit.  A solid cell
					// (all four quarters) is drawn as a colour block;
					// anything else as '#' in that colour.
					unsigned colour = (ch >> 4) & 7;
					out = (ch & 0x0F) == 0x0F ? ' ' : '#';
					pair = (ch & 0x0F) == 0x0F ? 12 + colour : 4 + colour;
				}
				else
				{
					unsigned code = ch & 0x3F;	// 6847 order: @A-Z[\]^_ then space..?
					out = (char)(code < 32 ? code + 64 : code);
					pair = (ch & 0x40) ? 1 : 2;	// bit 6 = 0 -> inverse
				}
				attron(COLOR_PAIR(pair));
				mvaddch(top + r, left + c, out);
				attroff(COLOR_PAIR(pair));
			}
		}
	}

	DragonRegs reg;
	dragon_regs(&reg);
	attron(COLOR_PAIR(3));
	mvprintw(top + 17, left - 1, "PC:%04X  A:%02X B:%02X X:%04X Y:%04X S:%04X  TIMER:%u",
		 reg.pc, reg.a, reg.b, reg.x, reg.y, reg.s, (mem[0x112] << 8) | mem[0x113]);
	if (dragon_tape_inserted())
	{
		mvprintw(top + 18, left - 1, "TAPE: %.30s %u.%u/%u.%us %s motor %s     ", tapefile_name(),
			 dragon_tape_pos_ms() / 1000, dragon_tape_pos_ms() % 1000 / 100,
			 dragon_tape_len_ms() / 1000, dragon_tape_len_ms() % 1000 / 100,
			 dragon_tape_playing() ? "PLAY" : "STOP", dragon_tape_motor() ? "ON " : "OFF");
	}
	else
	{
		mvprintw(top + 18, left - 1, "TAPE: none (--tape FILE.cas/.wav)%s", turbo ? "  TURBO" : "      ");
	}
	if (tapefile_last()[0])
	{
		mvprintw(top + 19, left - 1, "%-60.60s", tapefile_last());
	}
	mvprintw(top + 20, left - 1, "[F10]:Exit [F1]:Reset [F2]:Turbo [F4]:Rewind tape [ESC/F3]:Break");
	attroff(COLOR_PAIR(3));
	refresh();
}

// ---- Main ------------------------------------------------------------------------

static bool basic_ready(void)
{
	char screen[16 * 33 + 1];
	vdg_text(dragon_memory(), screen);
	return strstr(screen, "OK ") != NULL || strstr(screen, "OK\n") != NULL;
}

static void type_key(int ch)
{
	char s[2] = { (char)ch, 0 };
	dragon_type(s);
}

int main(int argc, char **argv)
{
	long steps = 150;
	bool quiet = false;
	bool interactive = false;
	const char *inject = NULL;
	const char *rom_file = "d32.rom";
	const char *tape_file = NULL;
	const char *rec_prefix = NULL;
	bool use_sdl = false;

	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) quiet = true;
		else if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "--interactive")) interactive = true;
		else if (!strcmp(argv[i], "--sdl")) use_sdl = true;
		else if (!strcmp(argv[i], "--inject") && i + 1 < argc) inject = argv[++i];
		else if (!strcmp(argv[i], "--tape") && i + 1 < argc) tape_file = argv[++i];
		else if (!strcmp(argv[i], "--record") && i + 1 < argc) rec_prefix = argv[++i];
		else if (isdigit((unsigned char)argv[i][0])) steps = atol(argv[i]);
		else rom_file = argv[i];
	}
	if (argc == 1 && isatty(STDIN_FILENO))
	{
		interactive = true;
	}

	static uint8_t rom[DRAGON_ROM_SIZE];
	FILE *f = fopen(rom_file, "rb");
	if (!f)
	{
		perror("Failed to open ROM file");
		return 1;
	}
	size_t got = fread(rom, 1, sizeof(rom), f);
	fclose(f);
	if (got != sizeof(rom))
	{
		fprintf(stderr, "ROM file %s is %zu bytes; a Dragon 32 ROM is %u bytes\n",
			rom_file, got, (unsigned)DRAGON_ROM_SIZE);
		return 1;
	}
	dragon_init(rom);
	if (tape_file && !tapefile_insert(tape_file))
	{
		return 1;
	}
	if (rec_prefix)
	{
		tapefile_record(rec_prefix);
	}

	if (use_sdl)
	{
#if HAS_SDL
		return sdl_main(argc, argv);
#else
		fprintf(stderr, "--sdl needs SDL2 (install libsdl2-dev and rebuild)\n");
		return 1;
#endif
	}

	if (interactive)
	{
		signal(SIGINT, cleanup_and_exit);
		signal(SIGTERM, cleanup_and_exit);
		initscr();
		raw();		// raw, not cbreak: Ctrl+Q (XON) and Ctrl+C reach getch()
		noecho();
		nodelay(stdscr, TRUE);
		keypad(stdscr, TRUE);
		set_escdelay(25);
		curs_set(0);
		ncurses_active = true;
		if (has_colors())
		{
			start_color();
			if (can_change_color())
			{
				// Redefine the base colours from the VDG's measured palette.
				init_color(COLOR_BLACK, vdg_palette[VDG_BLACK][0] * 1000 / 255,
					   vdg_palette[VDG_BLACK][1] * 1000 / 255,
					   vdg_palette[VDG_BLACK][2] * 1000 / 255);
				init_color(COLOR_RED, vdg_palette[VDG_RED][0] * 1000 / 255,
					   vdg_palette[VDG_RED][1] * 1000 / 255,
					   vdg_palette[VDG_RED][2] * 1000 / 255);
				init_color(COLOR_GREEN, vdg_palette[VDG_GREEN][0] * 1000 / 255,
					   vdg_palette[VDG_GREEN][1] * 1000 / 255,
					   vdg_palette[VDG_GREEN][2] * 1000 / 255);
				init_color(COLOR_YELLOW, vdg_palette[VDG_YELLOW][0] * 1000 / 255,
					   vdg_palette[VDG_YELLOW][1] * 1000 / 255,
					   vdg_palette[VDG_YELLOW][2] * 1000 / 255);
				init_color(COLOR_BLUE, vdg_palette[VDG_BLUE][0] * 1000 / 255,
					   vdg_palette[VDG_BLUE][1] * 1000 / 255,
					   vdg_palette[VDG_BLUE][2] * 1000 / 255);
				init_color(COLOR_MAGENTA, vdg_palette[VDG_MAGENTA][0] * 1000 / 255,
					   vdg_palette[VDG_MAGENTA][1] * 1000 / 255,
					   vdg_palette[VDG_MAGENTA][2] * 1000 / 255);
				init_color(COLOR_CYAN, vdg_palette[VDG_CYAN][0] * 1000 / 255,
					   vdg_palette[VDG_CYAN][1] * 1000 / 255,
					   vdg_palette[VDG_CYAN][2] * 1000 / 255);
			}
			init_pair(1, COLOR_BLACK, COLOR_GREEN);	// normal green screen
			init_pair(2, COLOR_GREEN, COLOR_BLACK);	// inverse text
			init_pair(3, COLOR_WHITE, COLOR_BLACK);	// chrome
			for (int i = 0; i < 8; i++)
			{
				// Semigraphics-4: colour i as a foreground or background
				init_pair(4 + i, sg_colour(i), COLOR_BLACK);
				init_pair(12 + i, COLOR_BLACK, sg_colour(i));
			}
		}

		bool running = true, turbo = false;
		while (running)
		{
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			dragon_run_frame();		// one 50 Hz frame at real Dragon speed
			draw_screen(turbo);

			int ch;
			while ((ch = getch()) != ERR)
			{
				if (ch == KEY_F(10) || ch == 17 || ch == 3) { running = false; break; }	// F10, Ctrl+Q, Ctrl+C
				else if (ch == KEY_F(1)) dragon_reset();
				else if (ch == KEY_F(2)) turbo = !turbo;
				else if (ch == KEY_F(4)) dragon_tape_seek_ms(0);
				else if (ch == 27 || ch == KEY_F(3)) dragon_type("\\b");		// BREAK
				else if (ch == KEY_HOME || ch == KEY_CLEAR || ch == 12) dragon_type("\\c");	// CLEAR
				else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8 || ch == KEY_LEFT)
					dragon_type_key(DRAGON_KEY_UP_ROW, DRAGON_KEY_LEFT_COL, false);	// left = delete
				else if (ch == KEY_UP) dragon_type_key(DRAGON_KEY_UP_ROW, DRAGON_KEY_UP_COL, false);
				else if (ch == KEY_DOWN) dragon_type_key(DRAGON_KEY_UP_ROW, DRAGON_KEY_DOWN_COL, false);
				else if (ch == KEY_RIGHT) dragon_type_key(DRAGON_KEY_UP_ROW, DRAGON_KEY_RIGHT_COL, false);
				else if (ch == '\n' || ch == '\r') type_key('\r');
				else if (ch >= 32 && ch <= 126) type_key(ch);
			}

			if (!turbo)
			{
				clock_gettime(CLOCK_MONOTONIC, &t1);
				long us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L;
				if (us < 20000L)
				{
					usleep((useconds_t)(20000L - us));
				}
			}
		}
		tapefile_finish();
		endwin();
		ncurses_active = false;
		printf("Dragon 32 emulator exited cleanly.\n");
		return 0;
	}

	// Batch / trace mode: run a number of instructions
	printf("Instruction limit (%ld)\n", steps);
	if (!quiet)
	{
		printf("PC\t\tINSTRUCTION\tOPERANDS\n");
	}
	bool typed = false;
	for (long i = 0; i < steps; i++)
	{
		if (inject && !typed && i % 10000 == 0 && i > 0 && basic_ready())
		{
			dragon_type(inject);	// BASIC is up: type the text
			typed = true;
		}
		dragon_step(!quiet);
	}
	tapefile_finish();

	DragonRegs r;
	dragon_regs(&r);
	printf("\nExecution completed (%ld instructions, %llu cycles):\n", steps,
	       (unsigned long long)dragon_cycles());
	printf("Final PC: %04X\n", r.pc);
	print_regs();
	dump_screen();
	return 0;
}
