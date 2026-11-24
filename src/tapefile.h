/*
 * tapefile.h - cassette tape files as FILE* (shared by the ncurses and
 * SDL front ends; the tape deck itself lives in the core)
 */
#ifndef TAPEFILE_H
#define TAPEFILE_H

#include <stdbool.h>

// Insert a tape (--tape FILE.cas/.wav); false if unusable.
bool tapefile_insert(const char *path);
const char *tapefile_name(void);

// Record CSAVE output (--record PREFIX) to PREFIX.wav + PREFIX.cas.
void tapefile_record(const char *prefix);
// Last "saved ..." message, or "".
const char *tapefile_last(void);

// Flush any open recording (eject, shutdown).
void tapefile_finish(void);

#endif
