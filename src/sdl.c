/*
 * sdl.c - the Dragon 32 in an SDL2 window: 256 x 192 scaled up, the VDG
 * picture from the core's vdg_render(), sound, and the Dragon keyboard
 * matrix fed from SDL keys.
 *
 * Copyright (C) 2026 Dylan Jones
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The emulation itself is the shared core in src/core/; this file is only
 * the SDL window around it (like src/main.c is the ncurses one).
 */
#include "core/dragon.h"
#include "tapefile.h"

#include <SDL2/SDL.h>

#include <stdio.h>
#include <string.h>

#define SCALE		3	// 256 x 192 -> 768 x 576
#define AUDIO_RATE	48000

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_AudioDeviceID audio_dev;
static uint32_t audio_obtained;

// Which SDL scancodes are held down; the Dragon matrix is rebuilt from it.
static bool held[SDL_NUM_SCANCODES];

// ---- Keyboard ---------------------------------------------------------------

// One SDL key, as the (row, col) of the Dragon matrix key plus whether the
// Dragon's Shift must be pressed too.  Special (non-character) Dragon keys
// are handled by the caller.
static bool key_to_dragon(int scancode, int *row, int *col, bool *shift)
{
	SDL_Keycode sym = SDL_GetKeyFromScancode((SDL_Scancode)scancode);
	*shift = false;

	switch (sym)
	{
		case SDLK_RETURN:
		case SDLK_KP_ENTER:
			*row = DRAGON_KEY_ENTER_ROW; *col = DRAGON_KEY_ENTER_COL; return true;
		case SDLK_BACKSPACE:
		case SDLK_LEFT:
			*row = DRAGON_KEY_UP_ROW; *col = DRAGON_KEY_LEFT_COL; return true;
		case SDLK_UP:
			*row = DRAGON_KEY_UP_ROW; *col = DRAGON_KEY_UP_COL; return true;
		case SDLK_DOWN:
			*row = DRAGON_KEY_UP_ROW; *col = DRAGON_KEY_DOWN_COL; return true;
		case SDLK_RIGHT:
			*row = DRAGON_KEY_UP_ROW; *col = DRAGON_KEY_RIGHT_COL; return true;
		case SDLK_LSHIFT:
		case SDLK_RSHIFT:
			*row = DRAGON_KEY_SHIFT_ROW; *col = DRAGON_KEY_SHIFT_COL; return true;
		default:
			break;
	}

	// Printable character: let the core map it (it knows the shifted
	// symbols too, so an unshifted key + Dragon Shift gives the symbol).
	char c = (char)sym;
	if (c >= 'a' && c <= 'z') c = (char)(c - 32);
	if (c >= 32 && c <= 126 && dragon_char_key(c, row, col, shift))
	{
		return true;
	}
	return false;
}

static void rebuild_matrix(void)
{
	uint8_t matrix[8];
	for (int i = 0; i < 8; i++) matrix[i] = 0x7F;
	for (int s = 0; s < SDL_NUM_SCANCODES; s++)
	{
		if (!held[s]) continue;
		int row, col;
		bool shift;
		if (key_to_dragon(s, &row, &col, &shift))
		{
			// Dragon Shift is matrix row 6, column 7
			if (row >= 0 && row < 7 && col >= 0 && col < 8)
			{
				matrix[col] &= (uint8_t)~(1 << row);
			}
		}
	}
	dragon_set_matrix(matrix);
}

// ---- Picture -----------------------------------------------------------------

static void upload_picture(void)
{
	const uint8_t *mem = dragon_memory();
	uint8_t pic[VDG_HEIGHT][VDG_WIDTH];
	vdg_render(mem, pic);

	static uint8_t pixels[VDG_HEIGHT][VDG_WIDTH * 3];
	for (int y = 0; y < VDG_HEIGHT; y++)
	{
		for (int x = 0; x < VDG_WIDTH; x++)
		{
			const uint8_t *rgb = vdg_palette[pic[y][x]];
			pixels[y][x * 3 + 0] = rgb[0];
			pixels[y][x * 3 + 1] = rgb[1];
			pixels[y][x * 3 + 2] = rgb[2];
		}
	}
	SDL_UpdateTexture(tex, NULL, pixels, VDG_WIDTH * 3);

	uint8_t border = vdg_border(mem);
	SDL_SetRenderDrawColor(ren, vdg_palette[border][0],
			       vdg_palette[border][1], vdg_palette[border][2], 255);
	SDL_RenderClear(ren);
	SDL_RenderCopy(ren, tex, NULL, NULL);
	SDL_RenderPresent(ren);
}

// ---- Audio -------------------------------------------------------------------

static void audio_queue(void)
{
	int16_t buf[4096];
	unsigned n = dragon_audio_take(buf, 4096);
	if (n && audio_dev != 0)
	{
		SDL_QueueAudio(audio_dev, buf, n * sizeof(int16_t));
	}
}

// ---- Main loop ---------------------------------------------------------------

int sdl_main(int argc, char **argv)
{
	(void)argc; (void)argv;

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
	{
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}

	win = SDL_CreateWindow("Dragon 32", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			       VDG_WIDTH * SCALE, VDG_HEIGHT * SCALE, 0);
	if (!win)
	{
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
	if (!ren)
	{
		ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
	}
	if (!ren)
	{
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}
	tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24,
				SDL_TEXTUREACCESS_STREAMING, VDG_WIDTH, VDG_HEIGHT);
	if (!tex)
	{
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		SDL_DestroyRenderer(ren);
		SDL_DestroyWindow(win);
		SDL_Quit();
		return 1;
	}

	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = AUDIO_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 1;
	want.samples = 1024;
	audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (audio_dev != 0)
	{
		dragon_audio_rate(have.freq);
		audio_obtained = have.freq;
		SDL_PauseAudioDevice(audio_dev, 0);
	}
	else
	{
		fprintf(stderr, "SDL audio: %s (running silently)\n", SDL_GetError());
		dragon_audio_rate(AUDIO_RATE);
		audio_obtained = 0;
	}

	bool turbo = false;
	bool running = true;
	uint32_t frame_start = SDL_GetTicks();
	while (running)
	{
		SDL_Event ev;
		while (SDL_PollEvent(&ev))
		{
			switch (ev.type)
			{
			case SDL_QUIT:
				running = false;
				break;
			case SDL_KEYDOWN:
				if (ev.key.repeat) break;
				if (ev.key.keysym.scancode >= 0 && ev.key.keysym.scancode < SDL_NUM_SCANCODES)
				{
					held[ev.key.keysym.scancode] = true;
				}
				rebuild_matrix();
				if (ev.key.keysym.sym == SDLK_F1) dragon_reset();
				else if (ev.key.keysym.sym == SDLK_F2) turbo = !turbo;
				else if (ev.key.keysym.sym == SDLK_F4) dragon_tape_seek_ms(0);
				else if (ev.key.keysym.sym == SDLK_F10) running = false;
				else if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_F3)
					dragon_type("\\b");			// BREAK
				else if (ev.key.keysym.sym == SDLK_HOME || ev.key.keysym.sym == SDLK_CLEAR)
					dragon_type("\\c");			// CLEAR
				break;
			case SDL_KEYUP:
				if (ev.key.keysym.scancode >= 0 && ev.key.keysym.scancode < SDL_NUM_SCANCODES)
				{
					held[ev.key.keysym.scancode] = false;
				}
				rebuild_matrix();
				break;
			}
		}

		dragon_run_frame();	// one 50 Hz frame at real Dragon speed
		upload_picture();
		audio_queue();

		if (!turbo)
		{
			// Pace to 50 Hz: each frame must take at least 20 ms.  Measure
			// from the start of the frame so the delay counts in the next
			// one, otherwise the loop runs about twice as fast as a Dragon.
			uint32_t now = SDL_GetTicks();
			uint32_t elapsed = now - frame_start;
			if (elapsed < 20)
			{
				SDL_Delay(20 - elapsed);
			}
			frame_start = SDL_GetTicks();
		}
		else
		{
			frame_start = SDL_GetTicks();
		}
	}

	tapefile_finish();
	if (audio_dev != 0) SDL_CloseAudioDevice(audio_dev);
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	printf("Dragon 32 emulator exited cleanly.\n");
	return 0;
}
