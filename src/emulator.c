/*
 * emulator.c - Motorola 6809 CPU emulator with Dragon 32 support
 *
 * Copyright (C) 2021-2026 Dylan Jones
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 * Tip: "mame dragon32 -debug" steps through the same ROM, handy for
 * comparing behaviour instruction by instruction.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <ncurses.h>

// This will contain the ROM
static unsigned char rom[16384];

// This will contain the RAM
static unsigned char ram[65536];

// Registers. These MUST stay static: ncurses exports its own global "PC"
// (termcap pad char) and writes to it in initscr(), which would otherwise
// overwrite the 6809 program counter and crash the full-screen mode.
static uint8_t A;			// 8-bit accumulator A
static uint8_t B;			// 8-bit accumulator B
static uint8_t CC;			// 8-bit Condition Code
static uint8_t DP;			// 8-bit Direct Page Register
static uint16_t X, Y;		// 16-bit index registers
static uint16_t U;			// 16-bit User stack pointer
static uint16_t S;			// 16-bit System stack pointer
static uint16_t PC;		// 16-bit Program Counter

// Condition Code (CC) flag bitmasks: E F H I N Z V C
#define CC_C 0x01
#define CC_V 0x02
#define CC_Z 0x04
#define CC_N 0x08
#define CC_I 0x10
#define CC_H 0x20
#define CC_F 0x40
#define CC_E 0x80

// Helper functions for 16-bit D accumulator (A:B)
static inline uint16_t get_D(void)
{
	return ((uint16_t)A << 8) | (uint16_t)B;
}

static inline void set_D(uint16_t val)
{
	A = (uint8_t)((val >> 8) & 0xFF);
	B = (uint8_t)(val & 0xFF);
}

// Memory Abstraction and ROM Write Protection

// Dragon 32 Keyboard Matrix & Input Emulation

static bool ncurses_active = false;

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

static uint8_t pia0_pb = 0xFF; // Columns output ($FF02)
static uint8_t key_matrix[8] = { 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F }; // Rows PA0..PA6 for each column (active low)

typedef struct {
	int row;
	int col;
	bool shift;
	char ch;
} KbdEvent;

#define KBD_QUEUE_SIZE 256
static KbdEvent kbd_queue[KBD_QUEUE_SIZE];
static int kbd_q_head = 0;
static int kbd_q_tail = 0;
static int kbd_hold_cycles = 0;
static int kbd_release_cycles = 0;

static void kbd_enqueue_event(int row, int col, bool shift, char ch)
{
	int next = (kbd_q_tail + 1) % KBD_QUEUE_SIZE;
	if (next != kbd_q_head)
	{
		kbd_queue[kbd_q_tail].row = row;
		kbd_queue[kbd_q_tail].col = col;
		kbd_queue[kbd_q_tail].shift = shift;
		kbd_queue[kbd_q_tail].ch = ch;
		kbd_q_tail = next;
	}
}

static bool kbd_map_char(char c, int *out_row, int *out_col, bool *out_shift);

static void kbd_enqueue_char(char c)
{
	int row = 0, col = 0;
	bool shift = false;
	if (kbd_map_char(c, &row, &col, &shift))
	{
		kbd_enqueue_event(row, col, shift, c);
	}
}

static void kbd_enqueue_special(int row, int col, bool shift)
{
	kbd_enqueue_event(row, col, shift, 0);
}

static void kbd_enqueue_string(const char *str)
{
	while (*str)
	{
		if (*str == '\\' && *(str + 1) == 'r')
		{
			kbd_enqueue_char('\r');
			str += 2;
		}
		else if (*str == '\\' && *(str + 1) == 'c')
		{
			kbd_enqueue_special(6, 1, false); // Dragon 32 CLEAR
			str += 2;
		}
		else if (*str == '\\' && *(str + 1) == 'b')
		{
			kbd_enqueue_special(6, 2, false); // Dragon 32 BREAK
			str += 2;
		}
		else if (*str == '\\' && *(str + 1) == 'n')
		{
			kbd_enqueue_char('\r');
			str += 2;
		}
		else
		{
			kbd_enqueue_char(*str++);
		}
	}
}

static void kbd_clear_matrix(void)
{
	for (int i = 0; i < 8; i++)
	{
		key_matrix[i] = 0x7F;
	}
}

static void kbd_set_key(int row, int col, bool shift)
{
	if (col >= 0 && col < 8 && row >= 0 && row < 7)
	{
		key_matrix[col] &= ~(1 << row);
	}
	if (shift)
	{
		key_matrix[7] &= ~(1 << 6); // Shift is at PB7, PA6
	}
}
static bool kbd_map_char(char c, int *out_row, int *out_col, bool *out_shift)
{
	*out_shift = false;
	if (c >= 'a' && c <= 'z') c = (char)(c - 32);

	switch (c)
	{
		case '0': *out_row = 0; *out_col = 0; return true;
		case '1': *out_row = 0; *out_col = 1; return true;
		case '2': *out_row = 0; *out_col = 2; return true;
		case '3': *out_row = 0; *out_col = 3; return true;
		case '4': *out_row = 0; *out_col = 4; return true;
		case '5': *out_row = 0; *out_col = 5; return true;
		case '6': *out_row = 0; *out_col = 6; return true;
		case '7': *out_row = 0; *out_col = 7; return true;

		case '8': *out_row = 1; *out_col = 0; return true;
		case '9': *out_row = 1; *out_col = 1; return true;
		case ':': *out_row = 1; *out_col = 2; return true;
		case ';': *out_row = 1; *out_col = 3; return true;
		case ',': *out_row = 1; *out_col = 4; return true;
		case '-': *out_row = 1; *out_col = 5; return true;
		case '.': *out_row = 1; *out_col = 6; return true;
		case '/': *out_row = 1; *out_col = 7; return true;

		case '@': *out_row = 2; *out_col = 0; return true;
		case 'A': *out_row = 2; *out_col = 1; return true;
		case 'B': *out_row = 2; *out_col = 2; return true;
		case 'C': *out_row = 2; *out_col = 3; return true;
		case 'D': *out_row = 2; *out_col = 4; return true;
		case 'E': *out_row = 2; *out_col = 5; return true;
		case 'F': *out_row = 2; *out_col = 6; return true;
		case 'G': *out_row = 2; *out_col = 7; return true;

		case 'H': *out_row = 3; *out_col = 0; return true;
		case 'I': *out_row = 3; *out_col = 1; return true;
		case 'J': *out_row = 3; *out_col = 2; return true;
		case 'K': *out_row = 3; *out_col = 3; return true;
		case 'L': *out_row = 3; *out_col = 4; return true;
		case 'M': *out_row = 3; *out_col = 5; return true;
		case 'N': *out_row = 3; *out_col = 6; return true;
		case 'O': *out_row = 3; *out_col = 7; return true;

		case 'P': *out_row = 4; *out_col = 0; return true;
		case 'Q': *out_row = 4; *out_col = 1; return true;
		case 'R': *out_row = 4; *out_col = 2; return true;
		case 'S': *out_row = 4; *out_col = 3; return true;
		case 'T': *out_row = 4; *out_col = 4; return true;
		case 'U': *out_row = 4; *out_col = 5; return true;
		case 'V': *out_row = 4; *out_col = 6; return true;
		case 'W': *out_row = 4; *out_col = 7; return true;

		case 'X': *out_row = 5; *out_col = 0; return true;
		case 'Y': *out_row = 5; *out_col = 1; return true;
		case 'Z': *out_row = 5; *out_col = 2; return true;
		case ' ': *out_row = 5; *out_col = 7; return true;

		case '\n':
		case '\r': *out_row = 6; *out_col = 0; return true;

		// Shifted Digits on Dragon 32 (subtract $10):
		case '!': *out_row = 0; *out_col = 1; *out_shift = true; return true; // Shift + 1
		case '"': *out_row = 0; *out_col = 2; *out_shift = true; return true; // Shift + 2
		case '#': *out_row = 0; *out_col = 3; *out_shift = true; return true; // Shift + 3
		case '$': *out_row = 0; *out_col = 4; *out_shift = true; return true; // Shift + 4
		case '%': *out_row = 0; *out_col = 5; *out_shift = true; return true; // Shift + 5
		case '&': *out_row = 0; *out_col = 6; *out_shift = true; return true; // Shift + 6
		case '\'': *out_row = 0; *out_col = 7; *out_shift = true; return true; // Shift + 7
		case '(': *out_row = 1; *out_col = 0; *out_shift = true; return true; // Shift + 8
		case ')': *out_row = 1; *out_col = 1; *out_shift = true; return true; // Shift + 9
		case '*': *out_row = 1; *out_col = 2; *out_shift = true; return true; // Shift + :
		case '+': *out_row = 1; *out_col = 3; *out_shift = true; return true; // Shift + ;

		// Shifted Punctuation (from Dragon 32 table at $BC84):
		case '<': *out_row = 1; *out_col = 4; *out_shift = true; return true; // Shift + ,
		case '=': *out_row = 1; *out_col = 5; *out_shift = true; return true; // Shift + -
		case '>': *out_row = 1; *out_col = 6; *out_shift = true; return true; // Shift + .
		case '?': *out_row = 1; *out_col = 7; *out_shift = true; return true; // Shift + /
		case '[': *out_row = 5; *out_col = 4; *out_shift = true; return true; // Shift + Down
		case ']': *out_row = 5; *out_col = 6; *out_shift = true; return true; // Shift + Right
		case '\\': *out_row = 6; *out_col = 1; *out_shift = true; return true; // Shift + Clear
		case '^': *out_row = 5; *out_col = 3; *out_shift = true; return true; // Shift + Up
		case '_': *out_row = 5; *out_col = 3; *out_shift = true; return true; // Shift + Up

		default: return false;
	}
}

static void kbd_update_cycles(int cycles)
{
	if (kbd_hold_cycles > 0)
	{
		kbd_hold_cycles -= cycles;
		if (kbd_hold_cycles <= 0)
		{
			kbd_clear_matrix();
			kbd_release_cycles = 15000;
		}
	}
	else if (kbd_release_cycles > 0)
	{
		kbd_release_cycles -= cycles;
	}
	else if (kbd_q_head != kbd_q_tail)
	{
		KbdEvent ev = kbd_queue[kbd_q_head];
		kbd_q_head = (kbd_q_head + 1) % KBD_QUEUE_SIZE;
		kbd_clear_matrix();
		kbd_set_key(ev.row, ev.col, ev.shift);
		kbd_hold_cycles = 25000;
	}
}

static inline uint8_t mem_read(uint16_t addr)
{
	if (addr == 0xFF00)
	{
		uint8_t rows = 0x7F;
		for (int col = 0; col < 8; col++)
		{
			if ((pia0_pb & (1 << col)) == 0)
			{
				rows &= key_matrix[col];
			}
		}
		return rows | 0x80;
	}
	if (addr == 0xFF02)
	{
		return pia0_pb;
	}
	return ram[addr];
}

static inline uint16_t mem_read16(uint16_t addr)
{
	return ((uint16_t)ram[addr] << 8) | ram[(uint16_t)(addr + 1)];
}

static inline void mem_write(uint16_t addr, uint8_t val)
{
	if (addr == 0xFF02)
	{
		pia0_pb = val;
		ram[addr] = val;
		return;
	}
	// Dragon 32 ROM Protection:
	// $8000-$BFFF: 16KB BASIC ROM
	// $FFF0-$FFFF: Vectors
	if ((addr >= 0x8000 && addr <= 0xBFFF) || addr >= 0xFFF0)
	{
		return;
	}
	ram[addr] = val;
}

static inline void mem_write16(uint16_t addr, uint16_t val)
{
	mem_write(addr, (uint8_t)(val >> 8));
	mem_write((uint16_t)(addr + 1), (uint8_t)(val & 0xFF));
}

// Flag helpers
static inline void set_flags_nz8(uint8_t res)
{
	CC &= ~(CC_N | CC_Z | CC_V);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
}

static inline void set_flags_nz16(uint16_t res)
{
	CC &= ~(CC_N | CC_Z | CC_V);
	if (res == 0) CC |= CC_Z;
	if (res & 0x8000) CC |= CC_N;
}

static inline void set_flags_logic8(uint8_t res)
{
	CC &= ~(CC_N | CC_Z | CC_V);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
}

static inline void set_flags_clr(void)
{
	CC &= ~(CC_N | CC_V | CC_C);
	CC |= CC_Z;
}

static inline uint8_t do_inc8(uint8_t val)
{
	uint8_t res = val + 1;
	CC &= ~(CC_N | CC_Z | CC_V);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (val == 0x7F) CC |= CC_V;
	return res;
}

static inline uint8_t do_dec8(uint8_t val)
{
	uint8_t res = val - 1;
	CC &= ~(CC_N | CC_Z | CC_V);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (val == 0x80) CC |= CC_V;
	return res;
}

static inline uint8_t do_com8(uint8_t val)
{
	uint8_t res = (uint8_t)~val;
	CC &= ~(CC_N | CC_Z | CC_V);
	CC |= CC_C;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	return res;
}

static inline uint8_t do_neg8(uint8_t val)
{
	uint8_t res = (uint8_t)(0 - val);
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (val == 0x80) CC |= CC_V;
	if (res != 0) CC |= CC_C;
	return res;
}

static inline uint8_t do_sub8(uint8_t r, uint8_t v)
{
	uint16_t diff = (uint16_t)r - (uint16_t)v;
	uint8_t res = (uint8_t)diff;
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (((r ^ v) & (r ^ res)) & 0x80) CC |= CC_V;
	if (diff & 0x100) CC |= CC_C;
	return res;
}

static inline uint8_t do_sbc8(uint8_t r, uint8_t v)
{
	uint8_t c = (CC & CC_C) ? 1 : 0;
	uint16_t diff = (uint16_t)r - (uint16_t)v - c;
	uint8_t res = (uint8_t)diff;
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (((r ^ v) & (r ^ res)) & 0x80) CC |= CC_V;
	if (diff & 0x100) CC |= CC_C;
	return res;
}

static inline uint8_t do_add8(uint8_t r, uint8_t v)
{
	uint16_t sum = (uint16_t)r + (uint16_t)v;
	uint8_t res = (uint8_t)sum;
	CC &= ~(CC_H | CC_N | CC_Z | CC_V | CC_C);
	if ((r ^ v ^ res) & 0x10) CC |= CC_H;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if ((~(r ^ v) & (r ^ res)) & 0x80) CC |= CC_V;
	if (sum & 0x100) CC |= CC_C;
	return res;
}

static inline uint8_t do_adc8(uint8_t r, uint8_t v)
{
	uint8_t c = (CC & CC_C) ? 1 : 0;
	uint16_t sum = (uint16_t)r + (uint16_t)v + c;
	uint8_t res = (uint8_t)sum;
	CC &= ~(CC_H | CC_N | CC_Z | CC_V | CC_C);
	if ((r ^ v ^ res) & 0x10) CC |= CC_H;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if ((~(r ^ v) & (r ^ res)) & 0x80) CC |= CC_V;
	if (sum & 0x100) CC |= CC_C;
	return res;
}

static inline void do_bit8(uint8_t r, uint8_t v)
{
	uint8_t res = r & v;
	set_flags_logic8(res);
}

static inline uint8_t do_lsr8(uint8_t val)
{
	uint8_t c = val & 0x01;
	uint8_t res = val >> 1;
	CC &= ~(CC_N | CC_Z | CC_C);
	if (c) CC |= CC_C;
	if (res == 0) CC |= CC_Z;
	return res;
}

static inline uint8_t do_ror8(uint8_t val)
{
	uint8_t old_c = (CC & CC_C) ? 0x80 : 0x00;
	uint8_t new_c = val & 0x01;
	uint8_t res = (val >> 1) | old_c;
	CC &= ~(CC_N | CC_Z | CC_C);
	if (new_c) CC |= CC_C;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	return res;
}

static inline uint8_t do_asr8(uint8_t val)
{
	uint8_t c = val & 0x01;
	uint8_t res = (uint8_t)(((int8_t)val) >> 1);
	CC &= ~(CC_N | CC_Z | CC_C);
	if (c) CC |= CC_C;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	return res;
}

static inline uint8_t do_asl8(uint8_t val)
{
	uint16_t res16 = (uint16_t)val << 1;
	uint8_t res = (uint8_t)res16;
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res16 & 0x100) CC |= CC_C;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (((val ^ res) & 0x80) != 0) CC |= CC_V;
	return res;
}

static inline uint8_t do_rol8(uint8_t val)
{
	uint8_t old_c = (CC & CC_C) ? 1 : 0;
	uint8_t new_c = (val & 0x80) ? 1 : 0;
	uint8_t res = (uint8_t)((val << 1) | old_c);
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (new_c) CC |= CC_C;
	if (res == 0) CC |= CC_Z;
	if (res & 0x80) CC |= CC_N;
	if (((val ^ res) & 0x80) != 0) CC |= CC_V;
	return res;
}

static inline uint16_t do_sub16(uint16_t a, uint16_t b)
{
	uint32_t diff = (uint32_t)a - (uint32_t)b;
	uint16_t res = (uint16_t)diff;
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res == 0) CC |= CC_Z;
	if (res & 0x8000) CC |= CC_N;
	if (((a ^ b) & (a ^ res)) & 0x8000) CC |= CC_V;
	if (diff & 0x10000) CC |= CC_C;
	return res;
}

static inline uint16_t do_subd(uint16_t val)
{
	uint16_t d = get_D();
	uint32_t diff = (uint32_t)d - (uint32_t)val;
	uint16_t res = (uint16_t)diff;
	set_D(res);
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res == 0) CC |= CC_Z;
	if (res & 0x8000) CC |= CC_N;
	if (((d ^ val) & (d ^ res)) & 0x8000) CC |= CC_V;
	if (diff & 0x10000) CC |= CC_C;
	return res;
}

static inline uint16_t do_addd(uint16_t val)
{
	uint16_t d = get_D();
	uint32_t sum = (uint32_t)d + (uint32_t)val;
	uint16_t res = (uint16_t)sum;
	set_D(res);
	CC &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (res == 0) CC |= CC_Z;
	if (res & 0x8000) CC |= CC_N;
	if ((~(d ^ val) & (d ^ res)) & 0x8000) CC |= CC_V;
	if (sum & 0x10000) CC |= CC_C;
	return res;
}

static inline void do_branch(bool condition, const char *name, bool quiet)
{
	int8_t offset = (int8_t)ram[PC++];
	uint16_t target = (uint16_t)(PC + (int16_t)offset);
	if (!quiet)
	{
		printf("\t%s [%02X]\t$%04X", name, (uint8_t)offset, target);
		if (condition) printf(" (taken)"); else printf(" (not taken)");
	}
	if (condition)
	{
		PC = target;
	}
}

static inline void do_lbranch(bool condition, const char *name, bool quiet)
{
	int16_t offset = (int16_t)(((uint16_t)ram[PC] << 8) | ram[PC+1]);
	PC += 2;
	uint16_t target = (uint16_t)(PC + offset);
	if (!quiet)
	{
		printf("\t%s\t$%04X", name, target);
		if (condition) printf(" (taken)"); else printf(" (not taken)");
	}
	if (condition)
	{
		PC = target;
	}
}

// 6809 Stack Push / Pull Helpers
static inline void push_s8(uint8_t val)
{
	S--;
	mem_write(S, val);
}

static inline void push_s16(uint16_t val)
{
	S--;
	mem_write(S, (uint8_t)(val & 0xFF));
	S--;
	mem_write(S, (uint8_t)((val >> 8) & 0xFF));
}

static inline uint8_t pull_s8(void)
{
	return mem_read(S++);
}

static inline uint16_t pull_s16(void)
{
	uint8_t hi = mem_read(S++);
	uint8_t lo = mem_read(S++);
	return ((uint16_t)hi << 8) | lo;
}

static inline void push_u8(uint8_t val)
{
	U--;
	mem_write(U, val);
}

static inline void push_u16(uint16_t val)
{
	U--;
	mem_write(U, (uint8_t)(val & 0xFF));
	U--;
	mem_write(U, (uint8_t)((val >> 8) & 0xFF));
}

static inline uint8_t pull_u8(void)
{
	return mem_read(U++);
}

static inline uint16_t pull_u16(void)
{
	uint8_t hi = mem_read(U++);
	uint8_t lo = mem_read(U++);
	return ((uint16_t)hi << 8) | lo;
}

static void do_pshs(uint8_t mask, char *disasm, size_t maxlen)
{
	char list[64] = "";
	if (mask & 0x80) { push_s16(PC); strcat(list, "PC,"); }
	if (mask & 0x40) { push_s16(U);  strcat(list, "U,"); }
	if (mask & 0x20) { push_s16(Y);  strcat(list, "Y,"); }
	if (mask & 0x10) { push_s16(X);  strcat(list, "X,"); }
	if (mask & 0x08) { push_s8(DP);  strcat(list, "DP,"); }
	if (mask & 0x04) { push_s8(B);   strcat(list, "B,"); }
	if (mask & 0x02) { push_s8(A);   strcat(list, "A,"); }
	if (mask & 0x01) { push_s8(CC);  strcat(list, "CC,"); }
	size_t len = strlen(list);
	if (len > 0 && list[len - 1] == ',') list[len - 1] = '\0';
	if (disasm) snprintf(disasm, maxlen, "%s", list);
}

static void do_puls(uint8_t mask, char *disasm, size_t maxlen)
{
	char list[64] = "";
	if (mask & 0x01) { CC = pull_s8();  strcat(list, "CC,"); }
	if (mask & 0x02) { A  = pull_s8();  strcat(list, "A,"); }
	if (mask & 0x04) { B  = pull_s8();  strcat(list, "B,"); }
	if (mask & 0x08) { DP = pull_s8();  strcat(list, "DP,"); }
	if (mask & 0x10) { X  = pull_s16(); strcat(list, "X,"); }
	if (mask & 0x20) { Y  = pull_s16(); strcat(list, "Y,"); }
	if (mask & 0x40) { U  = pull_s16(); strcat(list, "U,"); }
	if (mask & 0x80) { PC = pull_s16(); strcat(list, "PC,"); }
	size_t len = strlen(list);
	if (len > 0 && list[len - 1] == ',') list[len - 1] = '\0';
	if (disasm) snprintf(disasm, maxlen, "%s", list);
}

static void do_pshu(uint8_t mask, char *disasm, size_t maxlen)
{
	char list[64] = "";
	if (mask & 0x80) { push_u16(PC); strcat(list, "PC,"); }
	if (mask & 0x40) { push_u16(S);  strcat(list, "S,"); }
	if (mask & 0x20) { push_u16(Y);  strcat(list, "Y,"); }
	if (mask & 0x10) { push_u16(X);  strcat(list, "X,"); }
	if (mask & 0x08) { push_u8(DP);  strcat(list, "DP,"); }
	if (mask & 0x04) { push_u8(B);   strcat(list, "B,"); }
	if (mask & 0x02) { push_u8(A);   strcat(list, "A,"); }
	if (mask & 0x01) { push_u8(CC);  strcat(list, "CC,"); }
	size_t len = strlen(list);
	if (len > 0 && list[len - 1] == ',') list[len - 1] = '\0';
	if (disasm) snprintf(disasm, maxlen, "%s", list);
}

static void do_pulu(uint8_t mask, char *disasm, size_t maxlen)
{
	char list[64] = "";
	if (mask & 0x01) { CC = pull_u8();  strcat(list, "CC,"); }
	if (mask & 0x02) { A  = pull_u8();  strcat(list, "A,"); }
	if (mask & 0x04) { B  = pull_u8();  strcat(list, "B,"); }
	if (mask & 0x08) { DP = pull_u8();  strcat(list, "DP,"); }
	if (mask & 0x10) { X  = pull_u16(); strcat(list, "X,"); }
	if (mask & 0x20) { Y  = pull_u16(); strcat(list, "Y,"); }
	if (mask & 0x40) { S  = pull_u16(); strcat(list, "S,"); }
	if (mask & 0x80) { PC = pull_u16(); strcat(list, "PC,"); }
	size_t len = strlen(list);
	if (len > 0 && list[len - 1] == ',') list[len - 1] = '\0';
	if (disasm) snprintf(disasm, maxlen, "%s", list);
}

// 6809 TFR / EXG helpers
static inline const char* get_tfr_reg_name(int r)
{
	switch (r)
	{
		case 0x0: return "D";
		case 0x1: return "X";
		case 0x2: return "Y";
		case 0x3: return "U";
		case 0x4: return "S";
		case 0x5: return "PC";
		case 0x8: return "A";
		case 0x9: return "B";
		case 0xA: return "CC";
		case 0xB: return "DP";
		default: return "?";
	}
}

static inline uint16_t read_tfr_reg(int r)
{
	switch (r)
	{
		case 0x0: return get_D();
		case 0x1: return X;
		case 0x2: return Y;
		case 0x3: return U;
		case 0x4: return S;
		case 0x5: return PC;
		case 0x8: return A;
		case 0x9: return B;
		case 0xA: return CC;
		case 0xB: return DP;
		default: return 0;
	}
}

static inline void write_tfr_reg(int r, uint16_t val)
{
	switch (r)
	{
		case 0x0: set_D(val); break;
		case 0x1: X = val; break;
		case 0x2: Y = val; break;
		case 0x3: U = val; break;
		case 0x4: S = val; break;
		case 0x5: PC = val; break;
		case 0x8: A = (uint8_t)val; break;
		case 0x9: B = (uint8_t)val; break;
		case 0xA: CC = (uint8_t)val; break;
		case 0xB: DP = (uint8_t)val; break;
		default: break;
	}
}

static inline void do_tfr(char *disasm, size_t maxlen)
{
	uint8_t pb = ram[PC++];
	int r1 = (pb >> 4) & 0x0F;
	int r2 = pb & 0x0F;
	uint16_t val = read_tfr_reg(r1);
	write_tfr_reg(r2, val);
	if (disasm) snprintf(disasm, maxlen, "%s,%s", get_tfr_reg_name(r1), get_tfr_reg_name(r2));
}

static inline void do_exg(char *disasm, size_t maxlen)
{
	uint8_t pb = ram[PC++];
	int r1 = (pb >> 4) & 0x0F;
	int r2 = pb & 0x0F;
	uint16_t val1 = read_tfr_reg(r1);
	uint16_t val2 = read_tfr_reg(r2);
	write_tfr_reg(r1, val2);
	write_tfr_reg(r2, val1);
	if (disasm) snprintf(disasm, maxlen, "%s,%s", get_tfr_reg_name(r1), get_tfr_reg_name(r2));
}

static char *filename;

typedef struct
{
	int opcode;
	int cycles;
	int bytes;
	char *mnemonic;
	int addressmode;
	char *hexcode;
} Opcode;

static Opcode optable[256];

char *amode[7] = {"NULL      ","IMMEDIATE","DIRECT   ","INDEXED ","EXTENDED","INHERENT","RELATIVE  "};

static inline uint16_t* get_indexed_reg(int rr)
{
	switch (rr)
	{
		case 0: return &X;
		case 1: return &Y;
		case 2: return &U;
		case 3: return &S;
		default: return &X;
	}
}

static inline const char* get_indexed_reg_name(int rr)
{
	switch (rr)
	{
		case 0: return "X";
		case 1: return "Y";
		case 2: return "U";
		case 3: return "S";
		default: return "?";
	}
}

static uint16_t decode_indexed(char *disasm, size_t maxlen)
{
	uint8_t pb = ram[PC++];
	uint16_t ea = 0;

	if ((pb & 0x80) == 0)
	{
		int rr = (pb >> 5) & 0x03;
		uint16_t *reg = get_indexed_reg(rr);
		int8_t offset = (int8_t)(pb & 0x1F);
		if (offset & 0x10)
		{
			offset |= (int8_t)0xE0;
		}
		ea = (uint16_t)(*reg + (int16_t)offset);
		if (disasm)
		{
			snprintf(disasm, maxlen, "%d,%s", offset, get_indexed_reg_name(rr));
		}
		return ea;
	}

	int rr = (pb >> 5) & 0x03;
	uint16_t *reg = get_indexed_reg(rr);
	const char *rname = get_indexed_reg_name(rr);
	bool indirect = (pb & 0x10) != 0;
	int mode = pb & 0x0F;

	switch (mode)
	{
		case 0x00:
			ea = *reg;
			(*reg)++;
			if (disasm) snprintf(disasm, maxlen, ",%s+", rname);
			break;

		case 0x01:
			ea = *reg;
			(*reg) += 2;
			if (disasm) snprintf(disasm, maxlen, indirect ? "[,%s++]" : ",%s++", rname);
			break;

		case 0x02:
			(*reg)--;
			ea = *reg;
			if (disasm) snprintf(disasm, maxlen, ",-%s", rname);
			break;

		case 0x03:
			(*reg) -= 2;
			ea = *reg;
			if (disasm) snprintf(disasm, maxlen, indirect ? "[,--%s]" : ",--%s", rname);
			break;

		case 0x04:
			ea = *reg;
			if (disasm) snprintf(disasm, maxlen, indirect ? "[,%s]" : ",%s", rname);
			break;

		case 0x05:
			ea = (uint16_t)(*reg + (int16_t)(int8_t)B);
			if (disasm) snprintf(disasm, maxlen, indirect ? "[B,%s]" : "B,%s", rname);
			break;

		case 0x06:
			ea = (uint16_t)(*reg + (int16_t)(int8_t)A);
			if (disasm) snprintf(disasm, maxlen, indirect ? "[A,%s]" : "A,%s", rname);
			break;

		case 0x08:
		{
			int8_t off8 = (int8_t)ram[PC++];
			ea = (uint16_t)(*reg + (int16_t)off8);
			if (disasm) snprintf(disasm, maxlen, indirect ? "[%d,%s]" : "%d,%s", off8, rname);
			break;
		}

		case 0x09:
		{
			int16_t off16 = (int16_t)(((uint16_t)ram[PC] << 8) | ram[PC + 1]);
			PC += 2;
			ea = (uint16_t)(*reg + off16);
			if (disasm) snprintf(disasm, maxlen, indirect ? "[$%04X,%s]" : "$%04X,%s", (uint16_t)off16, rname);
			break;
		}

		case 0x0B:
			ea = (uint16_t)(*reg + (int16_t)get_D());
			if (disasm) snprintf(disasm, maxlen, indirect ? "[D,%s]" : "D,%s", rname);
			break;

		case 0x0C:
		{
			int8_t off8 = (int8_t)ram[PC++];
			ea = (uint16_t)(PC + (int16_t)off8);
			if (disasm) snprintf(disasm, maxlen, indirect ? "[$%04X,PCR]" : "$%04X,PCR", ea);
			break;
		}

		case 0x0D:
		{
			int16_t off16 = (int16_t)(((uint16_t)ram[PC] << 8) | ram[PC + 1]);
			PC += 2;
			ea = (uint16_t)(PC + off16);
			if (disasm) snprintf(disasm, maxlen, indirect ? "[$%04X,PCR]" : "$%04X,PCR", ea);
			break;
		}

		case 0x0F:
		{
			ea = ((uint16_t)ram[PC] << 8) | ram[PC + 1];
			PC += 2;
			if (disasm) snprintf(disasm, maxlen, "[$%04X]", ea);
			indirect = true;
			break;
		}

		default:
			ea = *reg;
			if (disasm) snprintf(disasm, maxlen, "?pb=%02X", pb);
			break;
	}

	if (indirect)
	{
		uint16_t indirect_ea = mem_read16(ea);
		return indirect_ea;
	}

	return ea;
}

static void importOpcodes(void)
{
	for (int i = 0; i < 256; i++)
	{
		optable[i].opcode = i;
		optable[i].cycles = 0;
		optable[i].bytes = 0;
		optable[i].mnemonic = "???";
		optable[i].addressmode = 0;
		optable[i].hexcode = "??";
	}
}

static void loadRomFile(void)
{
	printf("Loading ROM file %s\n", filename);
	FILE *ptr = fopen(filename, "rb");
	if (!ptr)
	{
		perror("Failed to open ROM file");
		exit(1);
	}
	size_t got = fread(rom, 1, sizeof(rom), ptr);
	if (got != sizeof(rom))
	{
		fprintf(stderr, "ROM file %s is %zu bytes; a Dragon 32 ROM is %zu bytes\n",
			filename, got, sizeof(rom));
		fclose(ptr);
		exit(1);
	}
	fclose(ptr);
}

static void loadRom(void)
{
	for (int c = 32768; c < 49152; c++)
	{
		ram[c] = rom[c - 32768];
	}
	// Mirror vectors ($BFF0-$BFFF) to high memory ($FFF0-$FFFF)
	for (int c = 0xFFF0; c <= 0xFFFF; c++)
	{
		ram[c] = rom[c - 0xC000];
	}
}

static void dump_screen(void)
{
	printf("\n--- DRAGON 32 TEXT SCREEN (0x0400 - 0x05FF) ---\n");
	printf("+--------------------------------+\n");
	for (int row = 0; row < 16; row++)
	{
		printf("|");
		for (int col = 0; col < 32; col++)
		{
			uint8_t ch = ram[0x0400 + row * 32 + col];
			// Motorola 6847 VDG character decoding:
			// 0x40-0x5F: Normal Uppercase Letters (@, A-Z)
			// 0x60-0x7F: Normal Digits, Punctuation & Space (ASCII 0x20-0x3F + 0x40)
			// 0x00-0x1F: Inverted Uppercase Letters
			// 0x20-0x3F: Inverted Digits, Punctuation & Space
			char c;
			if (ch >= 0x60 && ch <= 0x7F)
			{
				c = (char)(ch - 0x40);
			}
			else if (ch >= 0x40 && ch <= 0x5F)
			{
				c = (char)ch;
			}
			else if (ch <= 0x1F)
			{
				c = (char)(ch + 0x40);
			}
			else
			{
				c = (char)ch;
			}
			putchar(c);
		}
		printf("|\n");
	}
	printf("+--------------------------------+\n");
}


static void draw_ncurses_screen(void)
{
	int start_y = 1;
	int start_x = 2;

	// Draw border
	attron(COLOR_PAIR(3));
	mvprintw(start_y - 1, start_x - 1, "+--------------------------------+");
	for (int r = 0; r < 16; r++)
	{
		mvaddch(start_y + r, start_x - 1, '|');
		mvaddch(start_y + r, start_x + 32, '|');
	}
	mvprintw(start_y + 16, start_x - 1, "+--------------------------------+");
	attroff(COLOR_PAIR(3));

	// Draw 32x16 screen content
	for (int r = 0; r < 16; r++)
	{
		for (int c = 0; c < 32; c++)
		{
			uint8_t ch = ram[0x0400 + r * 32 + c];
			char ch_out;
			bool inverted = false;

			if (ch >= 0x60 && ch <= 0x7F)
			{
				ch_out = (char)(ch - 0x40);
			}
			else if (ch >= 0x40 && ch <= 0x5F)
			{
				ch_out = (char)ch;
			}
			else if (ch <= 0x1F)
			{
				ch_out = (char)(ch + 0x40);
				inverted = true;
			}
			else
			{
				ch_out = (char)ch;
				inverted = true;
			}

			if (inverted)
			{
				attron(COLOR_PAIR(2));
				mvaddch(start_y + r, start_x + c, ch_out);
				attroff(COLOR_PAIR(2));
			}
			else
			{
				attron(COLOR_PAIR(1));
				mvaddch(start_y + r, start_x + c, ch_out);
				attroff(COLOR_PAIR(1));
			}
		}
	}

	// Status bar
	attron(COLOR_PAIR(3));
	mvprintw(start_y + 17, start_x - 1, "PC:%04X  A:%02X B:%02X X:%04X Y:%04X S:%04X", PC, A, B, X, Y, S);
	mvprintw(start_y + 18, start_x - 1, "CC:[%c%c%c%c%c%c%c%c]  DP:%02X",
		(CC & CC_E) ? 'E' : '.',
		(CC & CC_F) ? 'F' : '.',
		(CC & CC_H) ? 'H' : '.',
		(CC & CC_I) ? 'I' : '.',
		(CC & CC_N) ? 'N' : '.',
		(CC & CC_Z) ? 'Z' : '.',
		(CC & CC_V) ? 'V' : '.',
		(CC & CC_C) ? 'C' : '.',
		DP);
	mvprintw(start_y + 19, start_x - 1, "[F10]:Exit [F1]:Reset [F2]:Turbo [ESC/F3]:Break");
	attroff(COLOR_PAIR(3));

	refresh();
}

static inline void step_cpu(bool quiet)
{
	bool executed = false;
if (!quiet)
		{
			printf("%04X\t", PC);
		}
		
		
		uint8_t op = ram[PC];
		switch (op)
		{
			// Direct mode single-operand instructions ($00-$0F)
			case 0x00: // NEG direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_neg8(mem_read(addr)));
				if (!quiet) printf("\tNEG [00]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x03: // COM direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_com8(mem_read(addr)));
				if (!quiet) printf("\tCOM [03]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x04: // LSR direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_lsr8(mem_read(addr)));
				if (!quiet) printf("\tLSR [04]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x06: // ROR direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_ror8(mem_read(addr)));
				if (!quiet) printf("\tROR [06]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x07: // ASR direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_asr8(mem_read(addr)));
				if (!quiet) printf("\tASR [07]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x08: // LSL direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_asl8(mem_read(addr)));
				if (!quiet) printf("\tLSL [08]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x09: // ROL direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_rol8(mem_read(addr)));
				if (!quiet) printf("\tROL [09]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x0A: // DEC direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_dec8(mem_read(addr)));
				if (!quiet) printf("\tDEC [0A]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x0C: // INC direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, do_inc8(mem_read(addr)));
				if (!quiet) printf("\tINC [0C]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x0D: // TST direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				set_flags_logic8(mem_read(addr));
				CC &= ~CC_V;
				if (!quiet) printf("\tTST [0D]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x0E: // JMP direct
			{
				PC++;
				uint16_t target = ((uint16_t)DP << 8) | ram[PC++];
				PC = target;
				if (!quiet) printf("\tJMP [0E]\t$%04X", target);
				executed = true;
				break;
			}
			case 0x0F: // CLR direct
			{
				PC++;
				uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
				mem_write(addr, 0);
				set_flags_clr();
				if (!quiet) printf("\tCLR [0F]\t$%04X", addr);
				executed = true;
				break;
			}

			// Page 2 Prefix Byte ($10)
			case 0x10:
			{
				PC++;
				uint8_t op2 = ram[PC++];
				switch (op2)
				{
					// Long branches ($10 $21 to $10 $2F)
					case 0x21: do_lbranch(false, "LBRN", quiet); executed = true; break;
					case 0x22: do_lbranch(!(CC & (CC_C | CC_Z)), "LBHI", quiet); executed = true; break;
					case 0x23: do_lbranch((CC & (CC_C | CC_Z)) != 0, "LBLS", quiet); executed = true; break;
					case 0x24: do_lbranch(!(CC & CC_C), "LBCC", quiet); executed = true; break;
					case 0x25: do_lbranch((CC & CC_C) != 0, "LBCS", quiet); executed = true; break;
					case 0x26: do_lbranch(!(CC & CC_Z), "LBNE", quiet); executed = true; break;
					case 0x27: do_lbranch((CC & CC_Z) != 0, "LBEQ", quiet); executed = true; break;
					case 0x28: do_lbranch(!(CC & CC_V), "LBVC", quiet); executed = true; break;
					case 0x29: do_lbranch((CC & CC_V) != 0, "LBVS", quiet); executed = true; break;
					case 0x2A: do_lbranch(!(CC & CC_N), "LBPL", quiet); executed = true; break;
					case 0x2B: do_lbranch((CC & CC_N) != 0, "LBMI", quiet); executed = true; break;
					case 0x2C: do_lbranch((!(((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))), "LBGE", quiet); executed = true; break;
					case 0x2D: do_lbranch(((((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))), "LBLT", quiet); executed = true; break;
					case 0x2E: do_lbranch((!(((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))) && !(CC & CC_Z), "LBGT", quiet); executed = true; break;
					case 0x2F: do_lbranch(((((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))) || (CC & CC_Z), "LBLE", quiet); executed = true; break;

					// 16-bit compares
					case 0x83: // CMPD immediate
					{
						uint16_t val16 = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						do_sub16(get_D(), val16);
						if (!quiet) printf("\tCMPD [1083]\t#$%04X", val16);
						executed = true;
						break;
					}
					case 0x93: // CMPD direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						uint16_t val16 = mem_read16(addr);
						do_sub16(get_D(), val16);
						if (!quiet) printf("\tCMPD [1093]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0xA3: // CMPD indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						uint16_t val16 = mem_read16(ea);
						do_sub16(get_D(), val16);
						if (!quiet) printf("\tCMPD [10A3]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xB3: // CMPD extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						uint16_t val16 = mem_read16(addr);
						do_sub16(get_D(), val16);
						if (!quiet) printf("\tCMPD [10B3]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0x8C: // CMPY immediate
					{
						uint16_t val16 = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						do_sub16(Y, val16);
						if (!quiet) printf("\tCMPY [108C]\t#$%04X", val16);
						executed = true;
						break;
					}
					case 0x9C: // CMPY direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						uint16_t val16 = mem_read16(addr);
						do_sub16(Y, val16);
						if (!quiet) printf("\tCMPY [109C]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0xAC: // CMPY indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						uint16_t val16 = mem_read16(ea);
						do_sub16(Y, val16);
						if (!quiet) printf("\tCMPY [10AC]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xBC: // CMPY extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						uint16_t val16 = mem_read16(addr);
						do_sub16(Y, val16);
						if (!quiet) printf("\tCMPY [10BC]\t$%04X", addr);
						executed = true;
						break;
					}

					// LDY
					case 0x8E: // LDY immediate
					{
						Y = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDY [108E]\t#$%04X", Y);
						executed = true;
						break;
					}
					case 0x9E: // LDY direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						Y = mem_read16(addr);
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDY [109E]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0xAE: // LDY indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						Y = mem_read16(ea);
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDY [10AE]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xBE: // LDY extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						Y = mem_read16(addr);
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDY [10BE]\t$%04X", addr);
						executed = true;
						break;
					}

					// STY
					case 0x9F: // STY direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						mem_write16(addr, Y);
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tSTY [109F]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0xAF: // STY indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						mem_write16(ea, Y);
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tSTY [10AF]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xBF: // STY extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						mem_write16(addr, Y);
						set_flags_nz16(Y);
						CC &= ~CC_V;
						if (!quiet) printf("\tSTY [10BF]\t$%04X", addr);
						executed = true;
						break;
					}

					// LDS
					case 0xCE: // LDS immediate
					{
						S = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDS [10CE]\t#$%04X", S);
						executed = true;
						break;
					}
					case 0xDE: // LDS direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						S = mem_read16(addr);
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDS [10DE]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0xEE: // LDS indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						S = mem_read16(ea);
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDS [10EE]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xFE: // LDS extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						S = mem_read16(addr);
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tLDS [10FE]\t$%04X", addr);
						executed = true;
						break;
					}

					// STS
					case 0xDF: // STS direct
					{
						uint16_t addr = ((uint16_t)DP << 8) | ram[PC++];
						mem_write16(addr, S);
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tSTS [10DF]\t$%04X", addr);
						executed = true;
						break;
					}
					case 0xEF: // STS indexed
					{
						char disasm[32];
						uint16_t ea = decode_indexed(disasm, sizeof(disasm));
						mem_write16(ea, S);
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tSTS [10EF]\t%s", disasm);
						executed = true;
						break;
					}
					case 0xFF: // STS extended
					{
						uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						mem_write16(addr, S);
						set_flags_nz16(S);
						CC &= ~CC_V;
						if (!quiet) printf("\tSTS [10FF]\t$%04X", addr);
						executed = true;
						break;
					}

					default:
						if (!quiet) printf("\t*unknown page 2 opcode 10 %02X*", op2);
						break;
				}
				break;
			}

			// Page 3 Prefix Byte ($11)
			case 0x11:
			{
				PC++;
				uint8_t op2 = ram[PC++];
				switch (op2)
				{
					case 0x3F: // SWI3
						if (!quiet) printf("\tSWI3 [113F]");
						executed = true;
						break;
					case 0x8C: // CMPU immediate
					{
						uint16_t val16 = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						do_sub16(U, val16);
						if (!quiet) printf("\tCMPU [118C]\t#$%04X", val16);
						executed = true;
						break;
					}
					case 0x8E: // CMPS immediate
					{
						uint16_t val16 = ((uint16_t)ram[PC] << 8) | ram[PC+1];
						PC += 2;
						do_sub16(S, val16);
						if (!quiet) printf("\tCMPS [118E]\t#$%04X", val16);
						executed = true;
						break;
					}
					default:
						if (!quiet) printf("\t*unknown page 3 opcode 11 %02X*", op2);
						break;
				}
				break;
			}

			case 0x12: // NOP
			{
				PC++;
				if (!quiet) printf("\tNOP [12]");
				executed = true;
				break;
			}
			case 0x13: // SYNC
			{
				PC++;
				if (!quiet) printf("\tSYNC [13]");
				executed = true;
				break;
			}
			case 0x16: // LBRA (Page 1)
			{
				PC++;
				do_lbranch(true, "LBRA", quiet);
				executed = true;
				break;
			}
			case 0x17: // LBSR (Page 1)
			{
				PC++;
				int16_t offset = (int16_t)(((uint16_t)ram[PC] << 8) | ram[PC+1]);
				PC += 2;
				uint16_t target = (uint16_t)(PC + offset);
				push_s16(PC);
				if (!quiet) printf("\tLBSR [17]\t$%04X", target);
				PC = target;
				executed = true;
				break;
			}
			case 0x19: // DAA
			{
				PC++;
				// Decimal Addition Adjust on accumulator A
				uint8_t msn = A >> 4;
				uint8_t lsn = A & 0x0F;
				uint8_t cf = 0;
				if ((CC & CC_C) || msn > 9 || (msn > 8 && lsn > 9)) cf = 0x60;
				if ((CC & CC_H) || lsn > 9) cf |= 0x06;
				uint16_t res = (uint16_t)A + (uint16_t)cf;
				A = (uint8_t)res;
				set_flags_nz8(A);
				if (res & 0x100) CC |= CC_C;
				if (!quiet) printf("\tDAA [19]");
				executed = true;
				break;
			}
			case 0x1A: // ORCC immediate
			{
				PC++;
				uint8_t val = ram[PC++];
				CC |= val;
				if (!quiet) printf("\tORCC [1A]\t#$%02X", val);
				executed = true;
				break;
			}
			case 0x1C: // ANDCC immediate
			{
				PC++;
				uint8_t val = ram[PC++];
				CC &= val;
				if (!quiet) printf("\tANDCC [1C]\t#$%02X", val);
				executed = true;
				break;
			}
			case 0x1D: // SEX (Sign Extend)
			{
				PC++;
				if (B & 0x80) A = 0xFF; else A = 0x00;
				set_flags_nz16(get_D());
				CC &= ~CC_V;
				if (!quiet) printf("\tSEX [1D]");
				executed = true;
				break;
			}
			case 0x1E: // EXG
			{
				PC++;
				char disasm[32];
				do_exg(disasm, sizeof(disasm));
				if (!quiet) printf("\tEXG [1E]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x1F: // TFR
			{
				PC++;
				char disasm[32];
				do_tfr(disasm, sizeof(disasm));
				if (!quiet) printf("\tTFR [1F]\t%s", disasm);
				executed = true;
				break;
			}

			// Page 1 Relative Branches ($20-$2F)
			case 0x20: PC++; do_branch(true, "BRA", quiet); executed = true; break;
			case 0x21: PC++; do_branch(false, "BRN", quiet); executed = true; break;
			case 0x22: PC++; do_branch(!(CC & (CC_C | CC_Z)), "BHI", quiet); executed = true; break;
			case 0x23: PC++; do_branch((CC & (CC_C | CC_Z)) != 0, "BLS", quiet); executed = true; break;
			case 0x24: PC++; do_branch(!(CC & CC_C), "BCC", quiet); executed = true; break;
			case 0x25: PC++; do_branch((CC & CC_C) != 0, "BCS", quiet); executed = true; break;
			case 0x26: PC++; do_branch(!(CC & CC_Z), "BNE", quiet); executed = true; break;
			case 0x27: PC++; do_branch((CC & CC_Z) != 0, "BEQ", quiet); executed = true; break;
			case 0x28: PC++; do_branch(!(CC & CC_V), "BVC", quiet); executed = true; break;
			case 0x29: PC++; do_branch((CC & CC_V) != 0, "BVS", quiet); executed = true; break;
			case 0x2A: PC++; do_branch(!(CC & CC_N), "BPL", quiet); executed = true; break;
			case 0x2B: PC++; do_branch((CC & CC_N) != 0, "BMI", quiet); executed = true; break;
			case 0x2C: PC++; do_branch((!(((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))), "BGE", quiet); executed = true; break;
			case 0x2D: PC++; do_branch(((((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))), "BLT", quiet); executed = true; break;
			case 0x2E: PC++; do_branch((!(((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))) && !(CC & CC_Z), "BGT", quiet); executed = true; break;
			case 0x2F: PC++; do_branch(((((CC & CC_N) != 0) ^ (((CC & CC_V) != 0)))) || (CC & CC_Z), "BLE", quiet); executed = true; break;

			// LEA instructions ($30-$33)
			case 0x30: // LEAX
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				X = ea;
				if (X == 0) CC |= CC_Z; else CC &= ~CC_Z;
				if (!quiet) printf("\tLEAX [30]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x31: // LEAY
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				Y = ea;
				if (Y == 0) CC |= CC_Z; else CC &= ~CC_Z;
				if (!quiet) printf("\tLEAY [31]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x32: // LEAS
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				S = ea;
				if (!quiet) printf("\tLEAS [32]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x33: // LEAU
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				U = ea;
				if (!quiet) printf("\tLEAU [33]\t%s", disasm);
				executed = true;
				break;
			}

			// Stack and Subroutine operations ($34-$3B)
			case 0x34: // PSHS
			{
				PC++;
				uint8_t mask = ram[PC++];
				char disasm[64];
				do_pshs(mask, disasm, sizeof(disasm));
				if (!quiet) printf("\tPSHS [34]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x35: // PULS
			{
				PC++;
				uint8_t mask = ram[PC++];
				char disasm[64];
				do_puls(mask, disasm, sizeof(disasm));
				if (!quiet) printf("\tPULS [35]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x36: // PSHU
			{
				PC++;
				uint8_t mask = ram[PC++];
				char disasm[64];
				do_pshu(mask, disasm, sizeof(disasm));
				if (!quiet) printf("\tPSHU [36]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x37: // PULU
			{
				PC++;
				uint8_t mask = ram[PC++];
				char disasm[64];
				do_pulu(mask, disasm, sizeof(disasm));
				if (!quiet) printf("\tPULU [37]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x39: // RTS
			{
				PC++;
				PC = pull_s16();
				if (!quiet) printf("\tRTS [39]\t-> $%04X", PC);
				executed = true;
				break;
			}
			case 0x3A: // ABX (Add B to X unsigned)
			{
				PC++;
				X += (uint16_t)B;
				if (!quiet) printf("\tABX [3A]");
				executed = true;
				break;
			}
			case 0x3B: // RTI (Return from Interrupt)
			{
				PC++;
				CC = pull_s8();
				if (CC & CC_E)
				{
					A = pull_s8();
					B = pull_s8();
					DP = pull_s8();
					X = pull_s16();
					Y = pull_s16();
					U = pull_s16();
				}
				PC = pull_s16();
				if (!quiet) printf("\tRTI [3B]\t-> $%04X", PC);
				executed = true;
				break;
			}
			case 0x3C: // CWAI
			{
				PC++;
				uint8_t val = ram[PC++];
				CC &= val;
				CC |= CC_E;
				push_s16(PC);
				push_s16(U);
				push_s16(Y);
				push_s16(X);
				push_s8(DP);
				push_s8(B);
				push_s8(A);
				push_s8(CC);
				if (!quiet) printf("\tCWAI [3C]");
				executed = true;
				break;
			}
			case 0x3D: // MUL
			{
				PC++;
				uint16_t prod = (uint16_t)A * (uint16_t)B;
				set_D(prod);
				CC &= ~(CC_Z | CC_C);
				if (prod == 0) CC |= CC_Z;
				if (prod & 0x80) CC |= CC_C;
				if (!quiet) printf("\tMUL [3D]");
				executed = true;
				break;
			}
			case 0x3F: // SWI
			{
				PC++;
				CC |= CC_E;
				push_s16(PC);
				push_s16(U);
				push_s16(Y);
				push_s16(X);
				push_s8(DP);
				push_s8(B);
				push_s8(A);
				push_s8(CC);
				CC |= (CC_I | CC_F);
				PC = mem_read16(0xFFF6);
				if (!quiet) printf("\tSWI [3F]\t-> $%04X", PC);
				executed = true;
				break;
			}

			// Inherent A operations ($40-$4F)
			case 0x40: PC++; A = do_neg8(A); if (!quiet) printf("\tNEGA [40]"); executed = true; break;
			case 0x43: PC++; A = do_com8(A); if (!quiet) printf("\tCOMA [43]"); executed = true; break;
			case 0x44: PC++; A = do_lsr8(A); if (!quiet) printf("\tLSRA [44]"); executed = true; break;
			case 0x46: PC++; A = do_ror8(A); if (!quiet) printf("\tRORA [46]"); executed = true; break;
			case 0x47: PC++; A = do_asr8(A); if (!quiet) printf("\tASRA [47]"); executed = true; break;
			case 0x48: PC++; A = do_asl8(A); if (!quiet) printf("\tASLA [48]"); executed = true; break;
			case 0x49: PC++; A = do_rol8(A); if (!quiet) printf("\tROLA [49]"); executed = true; break;
			case 0x4A: PC++; A = do_dec8(A); if (!quiet) printf("\tDECA [4A]"); executed = true; break;
			case 0x4C: PC++; A = do_inc8(A); if (!quiet) printf("\tINCA [4C]"); executed = true; break;
			case 0x4D: PC++; set_flags_logic8(A); CC &= ~CC_V; if (!quiet) printf("\tTSTA [4D]"); executed = true; break;
			case 0x4F: PC++; A = 0; set_flags_clr(); if (!quiet) printf("\tCLRA [4F]"); executed = true; break;

			// Inherent B operations ($50-$5F)
			case 0x50: PC++; B = do_neg8(B); if (!quiet) printf("\tNEGB [50]"); executed = true; break;
			case 0x53: PC++; B = do_com8(B); if (!quiet) printf("\tCOMB [53]"); executed = true; break;
			case 0x54: PC++; B = do_lsr8(B); if (!quiet) printf("\tLSRB [54]"); executed = true; break;
			case 0x56: PC++; B = do_ror8(B); if (!quiet) printf("\tRORB [56]"); executed = true; break;
			case 0x57: PC++; B = do_asr8(B); if (!quiet) printf("\tASRB [57]"); executed = true; break;
			case 0x58: PC++; B = do_asl8(B); if (!quiet) printf("\tASLB [58]"); executed = true; break;
			case 0x59: PC++; B = do_rol8(B); if (!quiet) printf("\tROLB [59]"); executed = true; break;
			case 0x5A: PC++; B = do_dec8(B); if (!quiet) printf("\tDECB [5A]"); executed = true; break;
			case 0x5C: PC++; B = do_inc8(B); if (!quiet) printf("\tINCB [5C]"); executed = true; break;
			case 0x5D: PC++; set_flags_logic8(B); CC &= ~CC_V; if (!quiet) printf("\tTSTB [5D]"); executed = true; break;
			case 0x5F: PC++; B = 0; set_flags_clr(); if (!quiet) printf("\tCLRB [5F]"); executed = true; break;

			// Indexed mode single-operand / control ($60-$6F)
			case 0x60: // NEG indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_neg8(mem_read(ea)));
				if (!quiet) printf("\tNEG [60]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x63: // COM indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_com8(mem_read(ea)));
				if (!quiet) printf("\tCOM [63]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x64: // LSR indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_lsr8(mem_read(ea)));
				if (!quiet) printf("\tLSR [64]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x66: // ROR indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_ror8(mem_read(ea)));
				if (!quiet) printf("\tROR [66]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x67: // ASR indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_asr8(mem_read(ea)));
				if (!quiet) printf("\tASR [67]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x68: // LSL indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_asl8(mem_read(ea)));
				if (!quiet) printf("\tLSL [68]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x69: // ROL indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_rol8(mem_read(ea)));
				if (!quiet) printf("\tROL [69]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x6A: // DEC indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_dec8(mem_read(ea)));
				if (!quiet) printf("\tDEC [6A]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x6C: // INC indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, do_inc8(mem_read(ea)));
				if (!quiet) printf("\tINC [6C]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x6D: // TST indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				set_flags_logic8(mem_read(ea));
				CC &= ~CC_V;
				if (!quiet) printf("\tTST [6D]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x6E: // JMP indexed
			{
				PC++;
				char disasm[32];
				uint16_t target = decode_indexed(disasm, sizeof(disasm));
				PC = target;
				if (!quiet) printf("\tJMP [6E]\t%s", disasm);
				executed = true;
				break;
			}
			case 0x6F: // CLR indexed
			{
				PC++;
				char disasm[32];
				uint16_t ea = decode_indexed(disasm, sizeof(disasm));
				mem_write(ea, 0);
				set_flags_clr();
				if (!quiet) printf("\tCLR [6F]\t%s", disasm);
				executed = true;
				break;
			}

			// Extended mode single-operand / control ($70-$7F)
			case 0x70: // NEG extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_neg8(mem_read(addr)));
				if (!quiet) printf("\tNEG [70]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x73: // COM extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_com8(mem_read(addr)));
				if (!quiet) printf("\tCOM [73]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x74: // LSR extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_lsr8(mem_read(addr)));
				if (!quiet) printf("\tLSR [74]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x76: // ROR extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_ror8(mem_read(addr)));
				if (!quiet) printf("\tROR [76]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x77: // ASR extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_asr8(mem_read(addr)));
				if (!quiet) printf("\tASR [77]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x78: // LSL extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_asl8(mem_read(addr)));
				if (!quiet) printf("\tLSL [78]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x79: // ROL extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_rol8(mem_read(addr)));
				if (!quiet) printf("\tROL [79]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x7A: // DEC extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_dec8(mem_read(addr)));
				if (!quiet) printf("\tDEC [7A]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x7C: // INC extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, do_inc8(mem_read(addr)));
				if (!quiet) printf("\tINC [7C]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x7D: // TST extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				set_flags_logic8(mem_read(addr));
				CC &= ~CC_V;
				if (!quiet) printf("\tTST [7D]\t$%04X", addr);
				executed = true;
				break;
			}
			case 0x7E: // JMP extended
			{
				PC++;
				uint16_t target = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC = target;
				if (!quiet) printf("\tJMP [7E]\t$%04X", target);
				executed = true;
				break;
			}
			case 0x7F: // CLR extended
			{
				PC++;
				uint16_t addr = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				mem_write(addr, 0);
				set_flags_clr();
				if (!quiet) printf("\tCLR [7F]\t$%04X", addr);
				executed = true;
				break;
			}

			// Immediate instructions ($80-$8E)
			case 0x80: PC++; A = do_sub8(A, ram[PC++]); if (!quiet) printf("\tSUBA [80]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x81: PC++; do_sub8(A, ram[PC++]); if (!quiet) printf("\tCMPA [81]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x82: PC++; A = do_sbc8(A, ram[PC++]); if (!quiet) printf("\tSBCA [82]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x83: { PC++; uint16_t v = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_subd(v); if (!quiet) printf("\tSUBD [83]\t#$%04X", v); executed = true; break; }
			case 0x84: PC++; A &= ram[PC++]; set_flags_logic8(A); if (!quiet) printf("\tANDA [84]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x85: PC++; do_bit8(A, ram[PC++]); if (!quiet) printf("\tBITA [85]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x86: PC++; A = ram[PC++]; set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tLDA [86]\t#$%02X", A); executed = true; break;
			case 0x88: PC++; A ^= ram[PC++]; set_flags_logic8(A); if (!quiet) printf("\tEORA [88]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x89: PC++; A = do_adc8(A, ram[PC++]); if (!quiet) printf("\tADCA [89]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x8A: PC++; A |= ram[PC++]; set_flags_logic8(A); if (!quiet) printf("\tORA [8A]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x8B: PC++; A = do_add8(A, ram[PC++]); if (!quiet) printf("\tADDA [8B]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0x8C: // CMPX immediate
			{
				PC++;
				uint16_t val16 = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				do_sub16(X, val16);
				if (!quiet) printf("\tCMPX [8C]\t#$%04X", val16);
				executed = true;
				break;
			}
			case 0x8D: // BSR
			{
				PC++;
				int8_t offset = (int8_t)ram[PC++];
				uint16_t target = (uint16_t)(PC + (int16_t)offset);
				push_s16(PC);
				if (!quiet) printf("\tBSR [8D]\t$%04X", target);
				PC = target;
				executed = true;
				break;
			}
			case 0x8E: // LDX immediate
			{
				PC++;
				X = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				set_flags_nz16(X);
				CC &= ~CC_V;
				if (!quiet) printf("\tLDX [8E]\t#$%04X", X);
				executed = true;
				break;
			}

			// Direct instructions ($90-$9F)
			case 0x90: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A = do_sub8(A, mem_read(ea)); if (!quiet) printf("\tSUBA [90]\t$%04X", ea); executed = true; break; }
			case 0x91: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_sub8(A, mem_read(ea)); if (!quiet) printf("\tCMPA [91]\t$%04X", ea); executed = true; break; }
			case 0x92: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A = do_sbc8(A, mem_read(ea)); if (!quiet) printf("\tSBCA [92]\t$%04X", ea); executed = true; break; }
			case 0x93: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_subd(mem_read16(ea)); if (!quiet) printf("\tSUBD [93]\t$%04X", ea); executed = true; break; }
			case 0x94: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A &= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tANDA [94]\t$%04X", ea); executed = true; break; }
			case 0x95: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_bit8(A, mem_read(ea)); if (!quiet) printf("\tBITA [95]\t$%04X", ea); executed = true; break; }
			case 0x96: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A = mem_read(ea); set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tLDA [96]\t$%04X", ea); executed = true; break; }
			case 0x97: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; mem_write(ea, A); set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tSTA [97]\t$%04X", ea); executed = true; break; }
			case 0x98: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A ^= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tEORA [98]\t$%04X", ea); executed = true; break; }
			case 0x99: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A = do_adc8(A, mem_read(ea)); if (!quiet) printf("\tADCA [99]\t$%04X", ea); executed = true; break; }
			case 0x9A: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A |= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tORA [9A]\t$%04X", ea); executed = true; break; }
			case 0x9B: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; A = do_add8(A, mem_read(ea)); if (!quiet) printf("\tADDA [9B]\t$%04X", ea); executed = true; break; }
			case 0x9C: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_sub16(X, mem_read16(ea)); if (!quiet) printf("\tCMPX [9C]\t$%04X", ea); executed = true; break; }
			case 0x9D: // JSR direct
			{
				PC++;
				uint16_t target = ((uint16_t)DP << 8) | ram[PC++];
				push_s16(PC);
				PC = target;
				if (!quiet) printf("\tJSR [9D]\t$%04X", target);
				executed = true;
				break;
			}
			case 0x9E: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; X = mem_read16(ea); set_flags_nz16(X); CC &= ~CC_V; if (!quiet) printf("\tLDX [9E]\t$%04X", ea); executed = true; break; }
			case 0x9F: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; mem_write16(ea, X); set_flags_nz16(X); CC &= ~CC_V; if (!quiet) printf("\tSTX [9F]\t$%04X", ea); executed = true; break; }

			// Indexed instructions ($A0-$AF)
			case 0xA0: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A = do_sub8(A, mem_read(ea)); if (!quiet) printf("\tSUBA [A0]\t%s", dis); executed = true; break; }
			case 0xA1: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_sub8(A, mem_read(ea)); if (!quiet) printf("\tCMPA [A1]\t%s", dis); executed = true; break; }
			case 0xA2: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A = do_sbc8(A, mem_read(ea)); if (!quiet) printf("\tSBCA [A2]\t%s", dis); executed = true; break; }
			case 0xA3: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_subd(mem_read16(ea)); if (!quiet) printf("\tSUBD [A3]\t%s", dis); executed = true; break; }
			case 0xA4: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A &= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tANDA [A4]\t%s", dis); executed = true; break; }
			case 0xA5: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_bit8(A, mem_read(ea)); if (!quiet) printf("\tBITA [A5]\t%s", dis); executed = true; break; }
			case 0xA6: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A = mem_read(ea); set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tLDA [A6]\t%s", dis); executed = true; break; }
			case 0xA7: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); mem_write(ea, A); set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tSTA [A7]\t%s", dis); executed = true; break; }
			case 0xA8: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A ^= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tEORA [A8]\t%s", dis); executed = true; break; }
			case 0xA9: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A = do_adc8(A, mem_read(ea)); if (!quiet) printf("\tADCA [A9]\t%s", dis); executed = true; break; }
			case 0xAA: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A |= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tORA [AA]\t%s", dis); executed = true; break; }
			case 0xAB: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); A = do_add8(A, mem_read(ea)); if (!quiet) printf("\tADDA [AB]\t%s", dis); executed = true; break; }
			case 0xAC: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_sub16(X, mem_read16(ea)); if (!quiet) printf("\tCMPX [AC]\t%s", dis); executed = true; break; }
			case 0xAD: // JSR indexed
			{
				PC++;
				char dis[32];
				uint16_t target = decode_indexed(dis, sizeof(dis));
				push_s16(PC);
				PC = target;
				if (!quiet) printf("\tJSR [AD]\t%s", dis);
				executed = true;
				break;
			}
			case 0xAE: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); X = mem_read16(ea); set_flags_nz16(X); CC &= ~CC_V; if (!quiet) printf("\tLDX [AE]\t%s", dis); executed = true; break; }
			case 0xAF: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); mem_write16(ea, X); set_flags_nz16(X); CC &= ~CC_V; if (!quiet) printf("\tSTX [AF]\t%s", dis); executed = true; break; }

			// Extended instructions ($B0-$BF)
			case 0xB0: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A = do_sub8(A, mem_read(ea)); if (!quiet) printf("\tSUBA [B0]\t$%04X", ea); executed = true; break; }
			case 0xB1: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_sub8(A, mem_read(ea)); if (!quiet) printf("\tCMPA [B1]\t$%04X", ea); executed = true; break; }
			case 0xB2: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A = do_sbc8(A, mem_read(ea)); if (!quiet) printf("\tSBCA [B2]\t$%04X", ea); executed = true; break; }
			case 0xB3: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_subd(mem_read16(ea)); if (!quiet) printf("\tSUBD [B3]\t$%04X", ea); executed = true; break; }
			case 0xB4: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A &= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tANDA [B4]\t$%04X", ea); executed = true; break; }
			case 0xB5: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_bit8(A, mem_read(ea)); if (!quiet) printf("\tBITA [B5]\t$%04X", ea); executed = true; break; }
			case 0xB6: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A = mem_read(ea); set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tLDA [B6]\t$%04X", ea); executed = true; break; }
			case 0xB7: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; mem_write(ea, A); set_flags_nz8(A); CC &= ~CC_V; if (!quiet) printf("\tSTA [B7]\t$%04X", ea); executed = true; break; }
			case 0xB8: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A ^= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tEORA [B8]\t$%04X", ea); executed = true; break; }
			case 0xB9: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A = do_adc8(A, mem_read(ea)); if (!quiet) printf("\tADCA [B9]\t$%04X", ea); executed = true; break; }
			case 0xBA: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A |= mem_read(ea); set_flags_logic8(A); if (!quiet) printf("\tORA [BA]\t$%04X", ea); executed = true; break; }
			case 0xBB: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; A = do_add8(A, mem_read(ea)); if (!quiet) printf("\tADDA [BB]\t$%04X", ea); executed = true; break; }
			case 0xBC: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_sub16(X, mem_read16(ea)); if (!quiet) printf("\tCMPX [BC]\t$%04X", ea); executed = true; break; }
			case 0xBD: // JSR extended
			{
				PC++;
				uint16_t target = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				push_s16(PC);
				PC = target;
				if (!quiet) printf("\tJSR [BD]\t$%04X", target);
				executed = true;
				break;
			}
			case 0xBE: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; X = mem_read16(ea); set_flags_nz16(X); CC &= ~CC_V; if (!quiet) printf("\tLDX [BE]\t$%04X", ea); executed = true; break; }
			case 0xBF: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; mem_write16(ea, X); set_flags_nz16(X); CC &= ~CC_V; if (!quiet) printf("\tSTX [BF]\t$%04X", ea); executed = true; break; }

			// Immediate instructions ($C0-$CE)
			case 0xC0: PC++; B = do_sub8(B, ram[PC++]); if (!quiet) printf("\tSUBB [C0]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xC1: PC++; do_sub8(B, ram[PC++]); if (!quiet) printf("\tCMPB [C1]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xC2: PC++; B = do_sbc8(B, ram[PC++]); if (!quiet) printf("\tSBCB [C2]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xC3: { PC++; uint16_t v = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_addd(v); if (!quiet) printf("\tADDD [C3]\t#$%04X", v); executed = true; break; }
			case 0xC4: PC++; B &= ram[PC++]; set_flags_logic8(B); if (!quiet) printf("\tANDB [C4]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xC5: PC++; do_bit8(B, ram[PC++]); if (!quiet) printf("\tBITB [C5]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xC6: PC++; B = ram[PC++]; set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tLDB [C6]\t#$%02X", B); executed = true; break;
			case 0xC8: PC++; B ^= ram[PC++]; set_flags_logic8(B); if (!quiet) printf("\tEORB [C8]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xC9: PC++; B = do_adc8(B, ram[PC++]); if (!quiet) printf("\tADCB [C9]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xCA: PC++; B |= ram[PC++]; set_flags_logic8(B); if (!quiet) printf("\tORB [CA]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xCB: PC++; B = do_add8(B, ram[PC++]); if (!quiet) printf("\tADDB [CB]\t#$%02X", ram[PC-1]); executed = true; break;
			case 0xCC: // LDD immediate
			{
				PC++;
				uint16_t val16 = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				set_D(val16);
				set_flags_nz16(val16);
				CC &= ~CC_V;
				if (!quiet) printf("\tLDD [CC]\t#$%04X", val16);
				executed = true;
				break;
			}
			case 0xCE: // LDU immediate
			{
				PC++;
				U = ((uint16_t)ram[PC] << 8) | ram[PC+1];
				PC += 2;
				set_flags_nz16(U);
				CC &= ~CC_V;
				if (!quiet) printf("\tLDU [CE]\t#$%04X", U);
				executed = true;
				break;
			}

			// Direct instructions ($D0-$DF)
			case 0xD0: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B = do_sub8(B, mem_read(ea)); if (!quiet) printf("\tSUBB [D0]\t$%04X", ea); executed = true; break; }
			case 0xD1: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_sub8(B, mem_read(ea)); if (!quiet) printf("\tCMPB [D1]\t$%04X", ea); executed = true; break; }
			case 0xD2: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B = do_sbc8(B, mem_read(ea)); if (!quiet) printf("\tSBCB [D2]\t$%04X", ea); executed = true; break; }
			case 0xD3: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_addd(mem_read16(ea)); if (!quiet) printf("\tADDD [D3]\t$%04X", ea); executed = true; break; }
			case 0xD4: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B &= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tANDB [D4]\t$%04X", ea); executed = true; break; }
			case 0xD5: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; do_bit8(B, mem_read(ea)); if (!quiet) printf("\tBITB [D5]\t$%04X", ea); executed = true; break; }
			case 0xD6: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B = mem_read(ea); set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tLDB [D6]\t$%04X", ea); executed = true; break; }
			case 0xD7: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; mem_write(ea, B); set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tSTB [D7]\t$%04X", ea); executed = true; break; }
			case 0xD8: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B ^= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tEORB [D8]\t$%04X", ea); executed = true; break; }
			case 0xD9: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B = do_adc8(B, mem_read(ea)); if (!quiet) printf("\tADCB [D9]\t$%04X", ea); executed = true; break; }
			case 0xDA: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B |= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tORB [DA]\t$%04X", ea); executed = true; break; }
			case 0xDB: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; B = do_add8(B, mem_read(ea)); if (!quiet) printf("\tADDB [DB]\t$%04X", ea); executed = true; break; }
			case 0xDC: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; uint16_t v = mem_read16(ea); set_D(v); set_flags_nz16(v); CC &= ~CC_V; if (!quiet) printf("\tLDD [DC]\t$%04X", ea); executed = true; break; }
			case 0xDD: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; mem_write16(ea, get_D()); set_flags_nz16(get_D()); CC &= ~CC_V; if (!quiet) printf("\tSTD [DD]\t$%04X", ea); executed = true; break; }
			case 0xDE: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; U = mem_read16(ea); set_flags_nz16(U); CC &= ~CC_V; if (!quiet) printf("\tLDU [DE]\t$%04X", ea); executed = true; break; }
			case 0xDF: { PC++; uint16_t ea = ((uint16_t)DP << 8) | ram[PC++]; mem_write16(ea, U); set_flags_nz16(U); CC &= ~CC_V; if (!quiet) printf("\tSTU [DF]\t$%04X", ea); executed = true; break; }

			// Indexed instructions ($E0-$EF)
			case 0xE0: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B = do_sub8(B, mem_read(ea)); if (!quiet) printf("\tSUBB [E0]\t%s", dis); executed = true; break; }
			case 0xE1: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_sub8(B, mem_read(ea)); if (!quiet) printf("\tCMPB [E1]\t%s", dis); executed = true; break; }
			case 0xE2: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B = do_sbc8(B, mem_read(ea)); if (!quiet) printf("\tSBCB [E2]\t%s", dis); executed = true; break; }
			case 0xE3: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_addd(mem_read16(ea)); if (!quiet) printf("\tADDD [E3]\t%s", dis); executed = true; break; }
			case 0xE4: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B &= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tANDB [E4]\t%s", dis); executed = true; break; }
			case 0xE5: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); do_bit8(B, mem_read(ea)); if (!quiet) printf("\tBITB [E5]\t%s", dis); executed = true; break; }
			case 0xE6: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B = mem_read(ea); set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tLDB [E6]\t%s", dis); executed = true; break; }
			case 0xE7: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); mem_write(ea, B); set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tSTB [E7]\t%s", dis); executed = true; break; }
			case 0xE8: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B ^= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tEORB [E8]\t%s", dis); executed = true; break; }
			case 0xE9: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B = do_adc8(B, mem_read(ea)); if (!quiet) printf("\tADCB [E9]\t%s", dis); executed = true; break; }
			case 0xEA: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B |= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tORB [EA]\t%s", dis); executed = true; break; }
			case 0xEB: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); B = do_add8(B, mem_read(ea)); if (!quiet) printf("\tADDB [EB]\t%s", dis); executed = true; break; }
			case 0xEC: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); uint16_t v = mem_read16(ea); set_D(v); set_flags_nz16(v); CC &= ~CC_V; if (!quiet) printf("\tLDD [EC]\t%s", dis); executed = true; break; }
			case 0xED: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); mem_write16(ea, get_D()); set_flags_nz16(get_D()); CC &= ~CC_V; if (!quiet) printf("\tSTD [ED]\t%s", dis); executed = true; break; }
			case 0xEE: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); U = mem_read16(ea); set_flags_nz16(U); CC &= ~CC_V; if (!quiet) printf("\tLDU [EE]\t%s", dis); executed = true; break; }
			case 0xEF: { PC++; char dis[32]; uint16_t ea = decode_indexed(dis, sizeof(dis)); mem_write16(ea, U); set_flags_nz16(U); CC &= ~CC_V; if (!quiet) printf("\tSTU [EF]\t%s", dis); executed = true; break; }

			// Extended instructions ($F0-$FF)
			case 0xF0: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B = do_sub8(B, mem_read(ea)); if (!quiet) printf("\tSUBB [F0]\t$%04X", ea); executed = true; break; }
			case 0xF1: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_sub8(B, mem_read(ea)); if (!quiet) printf("\tCMPB [F1]\t$%04X", ea); executed = true; break; }
			case 0xF2: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B = do_sbc8(B, mem_read(ea)); if (!quiet) printf("\tSBCB [F2]\t$%04X", ea); executed = true; break; }
			case 0xF3: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_addd(mem_read16(ea)); if (!quiet) printf("\tADDD [F3]\t$%04X", ea); executed = true; break; }
			case 0xF4: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B &= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tANDB [F4]\t$%04X", ea); executed = true; break; }
			case 0xF5: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; do_bit8(B, mem_read(ea)); if (!quiet) printf("\tBITB [F5]\t$%04X", ea); executed = true; break; }
			case 0xF6: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B = mem_read(ea); set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tLDB [F6]\t$%04X", ea); executed = true; break; }
			case 0xF7: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; mem_write(ea, B); set_flags_nz8(B); CC &= ~CC_V; if (!quiet) printf("\tSTB [F7]\t$%04X", ea); executed = true; break; }
			case 0xF8: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B ^= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tEORB [F8]\t$%04X", ea); executed = true; break; }
			case 0xF9: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B = do_adc8(B, mem_read(ea)); if (!quiet) printf("\tADCB [F9]\t$%04X", ea); executed = true; break; }
			case 0xFA: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B |= mem_read(ea); set_flags_logic8(B); if (!quiet) printf("\tORB [FA]\t$%04X", ea); executed = true; break; }
			case 0xFB: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; B = do_add8(B, mem_read(ea)); if (!quiet) printf("\tADDB [FB]\t$%04X", ea); executed = true; break; }
			case 0xFC: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; uint16_t v = mem_read16(ea); set_D(v); set_flags_nz16(v); CC &= ~CC_V; if (!quiet) printf("\tLDD [FC]\t$%04X", ea); executed = true; break; }
			case 0xFD: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; mem_write16(ea, get_D()); set_flags_nz16(get_D()); CC &= ~CC_V; if (!quiet) printf("\tSTD [FD]\t$%04X", ea); executed = true; break; }
			case 0xFE: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; U = mem_read16(ea); set_flags_nz16(U); CC &= ~CC_V; if (!quiet) printf("\tLDU [FE]\t$%04X", ea); executed = true; break; }
			case 0xFF: { PC++; uint16_t ea = ((uint16_t)ram[PC] << 8) | ram[PC+1]; PC += 2; mem_write16(ea, U); set_flags_nz16(U); CC &= ~CC_V; if (!quiet) printf("\tSTU [FF]\t$%04X", ea); executed = true; break; }

			default:
			{
				if (quiet)
				{
					printf("\nUnhandled opcode %02X at PC=%04X\n", ram[PC], PC);
				}
				else
				{
					printf("\t\t*default* [%02X]", ram[PC]);
				}
				PC++;
				break;
			}
		}
		
		if (!quiet)
		{
			if (executed)
			{
				printf(" - executed");
			}

			printf("\nA\tB\tD\tX\tY\tU\tS\tCC:EFHINZVC\n");
			printf("%02X\t%02X\t%04X\t%04X\t%04X\t%04X\t%04X\t%02X [%c%c%c%c%c%c%c%c]\n\n\n",
				A, B, get_D(), X, Y, U, S, CC,
				(CC & CC_E) ? 'E' : '.',
				(CC & CC_F) ? 'F' : '.',
				(CC & CC_H) ? 'H' : '.',
				(CC & CC_I) ? 'I' : '.',
				(CC & CC_N) ? 'N' : '.',
				(CC & CC_Z) ? 'Z' : '.',
				(CC & CC_V) ? 'V' : '.',
				(CC & CC_C) ? 'C' : '.');
		}
}

int main(int argc, char **argv)
{	
	importOpcodes();

	int cycleLimit = 150;
	bool quiet = false;
	bool interactive = false;
	char *inject_string = NULL;
   
	filename = "d32.rom";	
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0)
		{
			quiet = true;
		}
		else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0)
		{
			interactive = true;
		}
		else if (strcmp(argv[i], "--inject") == 0 && i + 1 < argc)
		{
			inject_string = argv[++i];
		}
		else if (isdigit((unsigned char)argv[i][0]))
		{
			cycleLimit = atoi(argv[i]);
		}
		else
		{
			filename = argv[i];
		}
	}

	// Auto-detect interactive mode when running in a terminal without cycle limit or quiet flag
	if (argc == 1 && isatty(STDIN_FILENO))
	{
		interactive = true;
	}

	loadRomFile();
	printf("Copying ROM to RAM\n");
	loadRom();

	// 6809 Reset Vector at $FFFE-$FFFF
	PC = ((uint16_t)ram[0xFFFE] << 8) | ram[0xFFFF];

	if (interactive)
	{
		signal(SIGINT, cleanup_and_exit);
		signal(SIGTERM, cleanup_and_exit);

		initscr();
		raw(); // raw, not cbreak: lets Ctrl+Q (XON) and Ctrl+C reach getch()
		noecho();
		nodelay(stdscr, TRUE);
		keypad(stdscr, TRUE);
		set_escdelay(25); // ESC (BREAK) responds at once instead of after 1 s
		curs_set(0);
		ncurses_active = true;

		if (has_colors())
		{
			start_color();
			init_pair(1, COLOR_BLACK, COLOR_GREEN); // Normal green screen
			init_pair(2, COLOR_GREEN, COLOR_BLACK); // Inverted text
			init_pair(3, COLOR_WHITE, COLOR_BLACK); // Chrome / Bezel
		}

		bool running = true;
		bool turbo = false;
		const int FRAME_CYCLES = 17800; // ~50 frames per second at 0.89 MHz

		while (running)
		{
			struct timespec t_start, t_end;
			clock_gettime(CLOCK_MONOTONIC, &t_start);

			for (int i = 0; i < FRAME_CYCLES; i++)
			{
				kbd_update_cycles(1);
				step_cpu(true);
			}

			draw_ncurses_screen();

			int ch = getch();
			while (ch != ERR)
			{
				if (ch == KEY_F(10) || ch == 17 || ch == 3) // F10, Ctrl+Q or Ctrl+C: exit
				{
					running = false;
					break;
				}
				else if (ch == KEY_F(1)) // F1: CPU Reset
				{
					PC = ((uint16_t)ram[0xFFFE] << 8) | ram[0xFFFF];
				}
				else if (ch == KEY_F(2)) // F2: Toggle Turbo mode
				{
					turbo = !turbo;
				}
				else if (ch == 27 || ch == KEY_F(3)) // ESC or F3: Dragon 32 BREAK key
				{
					kbd_enqueue_special(6, 2, false); // Row 6, Col 2: BREAK
				}
				else if (ch == KEY_HOME || ch == KEY_CLEAR || ch == 12) // Home or Ctrl+L: Dragon 32 CLEAR key
				{
					kbd_enqueue_special(6, 1, false); // Row 6, Col 1: CLEAR
				}
				else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8)
				{
					kbd_enqueue_special(5, 5, false); // Row 5, Col 5: Left/Backspace
				}
				else if (ch == KEY_UP)
				{
					kbd_enqueue_special(5, 3, false); // Row 5, Col 3: Up
				}
				else if (ch == KEY_DOWN)
				{
					kbd_enqueue_special(5, 4, false); // Row 5, Col 4: Down
				}
				else if (ch == KEY_LEFT)
				{
					kbd_enqueue_special(5, 5, false); // Row 5, Col 5: Left
				}
				else if (ch == KEY_RIGHT)
				{
					kbd_enqueue_special(5, 6, false); // Row 5, Col 6: Right
				}
				else if (ch == '\n' || ch == '\r')
				{
					kbd_enqueue_char('\r');
				}
				else if (ch >= 32 && ch <= 126)
				{
					kbd_enqueue_char((char)ch);
				}
				ch = getch();
			}

			if (!turbo)
			{
				clock_gettime(CLOCK_MONOTONIC, &t_end);
				long elapsed_us = (t_end.tv_sec - t_start.tv_sec) * 1000000L + (t_end.tv_nsec - t_start.tv_nsec) / 1000L;
				if (elapsed_us < 20000L)
				{
					usleep((useconds_t)(20000L - elapsed_us));
				}
			}
		}

		endwin();
		ncurses_active = false;
		printf("Dragon 32 emulator exited cleanly.\n");
		return 0;
	}

	// Non-interactive (batch / trace) mode
	printf("Cycle Limit (%i)\n", cycleLimit);
	printf("Now running ROM code starting at %04X\n", PC);
	if (!quiet)
	{
		printf("PC\t\tINSTRUCTION\tOPERANDS\n");
	}

	for (int c = 0; c < cycleLimit; c++)
	{
		kbd_update_cycles(1);
		if (c == 280000 && inject_string != NULL)
		{
			kbd_enqueue_string(inject_string);
		}
		step_cpu(quiet);
	}

	printf("\nExecution completed (%d cycles):\n", cycleLimit);
	printf("Final PC: %04X, Next Opcode: %02X\n", PC, ram[PC]);
	printf("A: %02X, B: %02X, D: %04X, X: %04X, Y: %04X, U: %04X, S: %04X, DP: %02X\n",
		A, B, get_D(), X, Y, U, S, DP);
	printf("CC: %02X [%c%c%c%c%c%c%c%c]\n", CC,
		(CC & CC_E) ? 'E' : '.',
		(CC & CC_F) ? 'F' : '.',
		(CC & CC_H) ? 'H' : '.',
		(CC & CC_I) ? 'I' : '.',
		(CC & CC_N) ? 'N' : '.',
		(CC & CC_Z) ? 'Z' : '.',
		(CC & CC_V) ? 'V' : '.',
		(CC & CC_C) ? 'C' : '.');

	dump_screen();
	return 0;
}
