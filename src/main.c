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
#include <ctype.h>
#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

// ---- Tape files ----------------------------------------------------------------

static FILE *tape_in;
static const char *tape_name;

static unsigned tape_read(void *ctx, uint32_t off, uint8_t *buf, unsigned len)
{
	(void)ctx;
	if (fseek(tape_in, off, SEEK_SET) != 0)
	{
		return 0;
	}
	return (unsigned)fread(buf, 1, len, tape_in);
}

static bool insert_tape(const char *path)
{
	tape_in = fopen(path, "rb");
	if (!tape_in)
	{
		perror(path);
		return false;
	}
	fseek(tape_in, 0, SEEK_END);
	uint32_t size = (uint32_t)ftell(tape_in);
	const char *dot = strrchr(path, '.');
	int kind = (dot && (!strcmp(dot, ".wav") || !strcmp(dot, ".WAV"))) ? DRAGON_TAPE_WAV : DRAGON_TAPE_CAS;
	if (!dragon_tape_insert(kind, size, tape_read, NULL))
	{
		fprintf(stderr, "%s: not a tape this can play\n", path);
		return false;
	}
	tape_name = path;
	return true;
}

static const char *rec_prefix;
static FILE *rec_wav, *rec_cas;
static char rec_last[300];

static void wav_header(FILE *f, uint32_t data)
{
	uint8_t h[44];
	uint32_t riff = data + 36, fmtlen = 16, rate = DRAGON_TAPE_WAV_RATE, byterate = DRAGON_TAPE_WAV_RATE;
	uint16_t pcm = 1, ch = 1, align = 1, bits = 8;
	memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4);
	memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &fmtlen, 4);
	memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4);
	memcpy(h + 28, &byterate, 4); memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
	memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
	fseek(f, 0, SEEK_SET);
	fwrite(h, 1, 44, f);
}

static void rec_begin(void *ctx)
{
	(void)ctx;
	char name[300];
	snprintf(name, sizeof(name), "%s.wav", rec_prefix);
	rec_wav = fopen(name, "wb");
	snprintf(name, sizeof(name), "%s.cas", rec_prefix);
	rec_cas = fopen(name, "wb");
	if (rec_wav)
	{
		wav_header(rec_wav, 0);
	}
}

static void rec_wavdata(void *ctx, const uint8_t *s, unsigned n)
{
	(void)ctx;
	if (rec_wav) fwrite(s, 1, n, rec_wav);
}

static void rec_casdata(void *ctx, const uint8_t *b, unsigned n)
{
	(void)ctx;
	if (rec_cas) fwrite(b, 1, n, rec_cas);
}

static void rec_end(void *ctx, const char *name)
{
	(void)ctx;
	if (rec_wav)
	{
		wav_header(rec_wav, (uint32_t)(ftell(rec_wav) - 44));
		fclose(rec_wav);
		rec_wav = NULL;
	}
	if (rec_cas)
	{
		fclose(rec_cas);
		rec_cas = NULL;
	}
	snprintf(rec_last, sizeof(rec_last), "saved \"%s\" to %s.wav + .cas", name, rec_prefix);
	if (!ncurses_active)
	{
		printf("Tape: %s\n", rec_last);
	}
}

static const DragonTapeRecorder recorder = { NULL, rec_begin, rec_wavdata, rec_casdata, rec_end };

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
				bool inverted = false;
				if (ch & 0x80)
				{
					out = '#';			// semigraphics block
				}
				else
				{
					unsigned code = ch & 0x3F;	// 6847 order: @A-Z[\]^_ then space..?
					out = (char)(code < 32 ? code + 64 : code);
					inverted = (ch & 0x40) == 0;
				}
				int pair = inverted ? 2 : 1;
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
		mvprintw(top + 18, left - 1, "TAPE: %.30s %u.%u/%u.%us %s motor %s     ", tape_name,
			 dragon_tape_pos_ms() / 1000, dragon_tape_pos_ms() % 1000 / 100,
			 dragon_tape_len_ms() / 1000, dragon_tape_len_ms() % 1000 / 100,
			 dragon_tape_playing() ? "PLAY" : "STOP", dragon_tape_motor() ? "ON " : "OFF");
	}
	else
	{
		mvprintw(top + 18, left - 1, "TAPE: none (--tape FILE.cas/.wav)%s", turbo ? "  TURBO" : "      ");
	}
	if (rec_last[0])
	{
		mvprintw(top + 19, left - 1, "%-60.60s", rec_last);
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

	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) quiet = true;
		else if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "--interactive")) interactive = true;
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
	if (tape_file && !insert_tape(tape_file))
	{
		return 1;
	}
	if (rec_prefix)
	{
		dragon_tape_recorder(&recorder);
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
			init_pair(1, COLOR_BLACK, COLOR_GREEN);	// normal green screen
			init_pair(2, COLOR_GREEN, COLOR_BLACK);	// inverse text
			init_pair(3, COLOR_WHITE, COLOR_BLACK);	// chrome
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
		dragon_tape_record_finish();
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
	dragon_tape_record_finish();

	DragonRegs r;
	dragon_regs(&r);
	printf("\nExecution completed (%ld instructions, %llu cycles):\n", steps,
	       (unsigned long long)dragon_cycles());
	printf("Final PC: %04X\n", r.pc);
	print_regs();
	dump_screen();
	return 0;
}
