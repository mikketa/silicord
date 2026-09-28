#pragma once

/*
 * Sound card I/O for voice: 48 kHz, 20 ms blocks, on a thread of its own.
 * `play` fills 960 stereo samples; `capture` (optional) receives 960 mono
 * samples from the microphone. Both run on the audio thread.
 */

#define AUDIO_NAME 32 /* device names, as Windows gives them */

typedef struct {
    void *ctx;
    void (*play)(void *ctx, float *out);
    void (*capture)(void *ctx, const float *in);
    unsigned out_device, in_device; /* 0 for the Windows default, else a device index + 1 */
} audio_io_t;

int audio_start(const audio_io_t *io);
void audio_stop(void);
/* The names of the output (or input) devices, in index order; returns how many. */
int audio_devices(int input, wchar_t (*names)[AUDIO_NAME], int max);
