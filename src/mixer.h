#pragma once
#include <stddef.h>

/*
 * The voice call's receive side: one Opus decoder and jitter buffer per
 * speaker, mixed into 20 ms blocks of 48 kHz stereo for the sound card.
 * Not thread-safe: the caller serializes push and pull.
 */

#define MIXER_BLOCK 960 /* samples per channel in 20 ms */

typedef struct mixer_voice mixer_voice_t;

typedef struct {
    mixer_voice_t **voices;
    int nvoices;
    float gain;    /* output volume, 1 by default */
    int deafened;  /* when set, nothing is played */
    float pcm[5760 * 2]; /* a decoded packet on its way to a speaker's queue */
} mixer_t;

void mixer_init(mixer_t *m);
void mixer_free(mixer_t *m);
/* An Opus packet from `user`, in RTP order: gaps in `seq` are concealed. */
void mixer_push(mixer_t *m, unsigned long long user, unsigned seq, const unsigned char *opus, size_t n);
/* The next 20 ms, interleaved stereo in [-1, 1]. */
void mixer_pull(mixer_t *m, float *out);
/* Drops a speaker who left. */
void mixer_remove(mixer_t *m, unsigned long long user);
/* A speaker's volume, 0 to 2 (1 by default). */
void mixer_set_volume(mixer_t *m, unsigned long long user, float volume);
/* Whether `user` was heard in the last 100 ms or so, for the speaking indicator. */
int mixer_speaking(const mixer_t *m, unsigned long long user);
