#pragma once

/*
 * Sound card I/O for voice: 48 kHz, 20 ms blocks, on a thread of its own.
 * `play` fills 960 stereo samples; `capture` (optional) receives 960 mono
 * samples from the default microphone. Both run on the audio thread.
 */

typedef struct {
    void *ctx;
    void (*play)(void *ctx, float *out);
    void (*capture)(void *ctx, const float *in);
} audio_io_t;

int audio_start(const audio_io_t *io);
void audio_stop(void);
