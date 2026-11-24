/*
 * tapefile.c - cassette tape files as FILE*: an input tape to play and a
 * recorder that saves what the Dragon writes.  Shared by the ncurses and
 * SDL front ends; the deck itself is the core's tape.c.
 *
 * Copyright (C) 2026 Dylan Jones
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "tapefile.h"
#include "core/dragon.h"
#include <stdio.h>
#include <string.h>

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

bool tapefile_insert(const char *path)
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

const char *tapefile_name(void)
{
	return tape_name;
}

// ---- Recording ---------------------------------------------------------------

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
	printf("Tape: %s\n", rec_last);
}

static const DragonTapeRecorder recorder = { NULL, rec_begin, rec_wavdata, rec_casdata, rec_end };

void tapefile_record(const char *prefix)
{
	rec_prefix = prefix;
	dragon_tape_recorder(&recorder);
}

const char *tapefile_last(void)
{
	return rec_last;
}

void tapefile_finish(void)
{
	dragon_tape_record_finish();
}
