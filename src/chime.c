#include <string.h>
#include "chime.h"
#include "mem.h"
#include "opus_math.h"

/* E4 F#4 A4 B4 C#5 E5 F#5 A5: whatever the order, no two notes clash. */
static const double k_scale[] = {329.63, 369.99, 440.00, 493.88, 554.37, 659.26, 739.99, 880.00};
/* A3 E4 A4 C#5 E5, and how loud each is. */
static const double k_chord[] = {220.00, 329.63, 440.00, 554.37, 659.26};
static const float k_chord_amp[] = {.03f, .035f, .04f, .035f, .03f};

/*
 * Adds a note: harmonics of `freq` (each weaker and shorter than the last),
 * a linear attack, then an exponential decay with time constant tau.
 */
static void note(float *pcm, int len, int start, double freq, float amp, const float *partials, int np, int attack_ms,
                 float tau_ms)
{
    int attack = attack_ms * CHIME_RATE / 1000;

    for (int k = 0; k < np; k++) {
        double w = 2 * OM_PI * freq * (k + 1) / CHIME_RATE, c = om_cos(w), s = om_sin(w), re = 1, im = 0;
        float tau = tau_ms / (float)(k + 1) * CHIME_RATE / 1000, env = 1;
        float decay = om_exp2(-1.442695f / tau), a = amp * partials[k];
        int end = start + (int)(tau * 9); /* down by 80 dB */

        if (2 * freq * (k + 1) >= CHIME_RATE)
            break;
        for (int i = start; i < end && i < len; i++) {
            double t = re * c - im * s;
            float ramp = i - start < attack ? (float)(i - start) / (float)attack : 1.f;
            im = re * s + im * c;
            re = t;
            pcm[i] += a * env * ramp * (float)im;
            if (i - start >= attack)
                env *= decay;
        }
    }
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, unsigned v)
{
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

unsigned char *chime_wav(const int *onsets, int n, int chord_ms, int end_ms, size_t *size)
{
    static const float pluck[] = {1.f, .35f, .12f}, bell[] = {1.f, .18f};
    int len = end_ms * CHIME_RATE / 1000, fade = CHIME_RATE / 12;
    float *pcm = mem_alloc((size_t)len * sizeof *pcm);
    unsigned char *wav = mem_alloc(44 + (size_t)len * 2);

    for (int i = 0; i < n; i++)
        note(pcm, len, onsets[i] * CHIME_RATE / 1000, k_scale[i % 8], .07f, pluck, 3, 3, 260.f);
    for (int i = 0; i < 5; i++) {
        int at = chord_ms * CHIME_RATE / 1000 + i * CHIME_RATE / 70; /* a quick strum, low to high */
        note(pcm, len, at, k_chord[i], k_chord_amp[i], bell, 2, 12, 700.f);
        note(pcm, len, at, k_chord[i] * 1.0035, k_chord_amp[i] * .5f, bell, 1, 12, 700.f); /* a slight shimmer */
    }

    memcpy(wav, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0", 24);
    put32(wav + 4, 36 + (unsigned)len * 2);
    put32(wav + 24, CHIME_RATE);
    put32(wav + 28, CHIME_RATE * 2);
    put16(wav + 32, 2);
    put16(wav + 34, 16);
    memcpy(wav + 36, "data", 4);
    put32(wav + 40, (unsigned)len * 2);
    for (int i = 0; i < len; i++) {
        float v = pcm[i] * (i >= len - fade ? (float)(len - i) / (float)fade : 1.f) * 32767.f;
        int s = (int)(v < -32767.f ? -32767.f : v > 32767.f ? 32767.f : v);
        put16(wav + 44 + 2 * i, (unsigned)s & 0xFFFF);
    }
    mem_free(pcm);
    *size = 44 + (size_t)len * 2;
    return wav;
}
