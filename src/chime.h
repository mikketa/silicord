#pragma once
#include <stddef.h>

/*
 * The startup chime, synthesized: a soft pluck at each onset (in ms),
 * climbing a major pentatonic scale, then a chord at `chord_ms` that rings
 * out until `end_ms`. Returns a 16-bit mono WAV file (mem_alloc'ed) and
 * its size in *size.
 */

#define CHIME_RATE 22050

unsigned char *chime_wav(const int *onsets, int n, int chord_ms, int end_ms, size_t *size);
