#include <string.h>
#include "mixer.h"
#include "mem.h"
#include "opus.h"

#define FIFO_BLOCKS 8  /* at most 160 ms queued per speaker */
#define START_BLOCKS 2 /* 40 ms buffered before a speaker is heard */
#define FIFO_LEN (FIFO_BLOCKS * MIXER_BLOCK)
#define MAX_CONCEALED 5 /* gaps longer than 100 ms restart the stream instead */
#define LOUD 1e-4f      /* mean square of a block that counts as speech */

struct mixer_voice {
    unsigned long long user;
    opus_decoder_t dec;
    float fifo[FIFO_LEN * 2];
    int head, count; /* in stereo samples */
    int started, have_seq;
    unsigned last_seq;
    float volume;
    int level;
};

void mixer_init(mixer_t *m)
{
    memset(m, 0, sizeof *m);
    m->gain = 1.f;
}

void mixer_free(mixer_t *m)
{
    for (int i = 0; i < m->nvoices; i++)
        mem_free(m->voices[i]);
    mem_free(m->voices);
    memset(m, 0, sizeof *m);
}

static mixer_voice_t *find(const mixer_t *m, unsigned long long user)
{
    for (int i = 0; i < m->nvoices; i++)
        if (m->voices[i]->user == user)
            return m->voices[i];
    return NULL;
}

static mixer_voice_t *voice(mixer_t *m, unsigned long long user)
{
    mixer_voice_t *v = find(m, user);

    if (v)
        return v;
    v = mem_alloc(sizeof *v);
    v->user = user;
    v->volume = 1.f;
    opus_decoder_init(&v->dec, 2);
    m->voices = mem_realloc(m->voices, sizeof(mixer_voice_t *) * (size_t)(m->nvoices + 1));
    m->voices[m->nvoices++] = v;
    return v;
}

/* Appends decoded samples, dropping the oldest when the queue is full (the speaker drifted ahead). */
static void enqueue(mixer_voice_t *v, const float *pcm, int n)
{
    for (int i = 0; i < n; i++) {
        int at;
        if (v->count == FIFO_LEN) {
            v->head = (v->head + 1) % FIFO_LEN;
            v->count--;
        }
        at = (v->head + v->count) % FIFO_LEN;
        v->fifo[2 * at] = pcm[2 * i];
        v->fifo[2 * at + 1] = pcm[2 * i + 1];
        v->count++;
    }
}

void mixer_push(mixer_t *m, unsigned long long user, unsigned seq, const unsigned char *opus, size_t n)
{
    mixer_voice_t *v = voice(m, user);
    int got;

    seq &= 0xFFFF;
    if (v->have_seq) {
        unsigned gap = (seq - v->last_seq) & 0xFFFF;
        if (gap == 0 || gap > 0x8000)
            return; /* a duplicate or a late packet */
        if (gap > 1 && gap - 1 <= MAX_CONCEALED) {
            /* Conceal the lost packets, a frame of the last duration each. */
            for (unsigned k = 0; k < gap - 1; k++) {
                got = opus_decode(&v->dec, NULL, 0, m->pcm, v->dec.frame_size);
                if (got > 0)
                    enqueue(v, m->pcm, got);
            }
        }
    }
    v->last_seq = seq;
    v->have_seq = 1;
    got = opus_decode(&v->dec, opus, n, m->pcm, 0);
    if (got > 0)
        enqueue(v, m->pcm, got);
}

void mixer_pull(mixer_t *m, float *out)
{
    memset(out, 0, sizeof(float) * 2 * MIXER_BLOCK);
    for (int i = 0; i < m->nvoices; i++) {
        mixer_voice_t *v = m->voices[i];
        float energy = 0, g = v->volume * m->gain;
        int take;
        if (!v->started && v->count >= START_BLOCKS * MIXER_BLOCK)
            v->started = 1;
        if (!v->started) {
            if (v->level > 0)
                v->level--;
            continue;
        }
        take = v->count < MIXER_BLOCK ? v->count : MIXER_BLOCK;
        for (int k = 0; k < take; k++) {
            int at = (v->head + k) % FIFO_LEN;
            float l = v->fifo[2 * at], r = v->fifo[2 * at + 1];
            energy += l * l + r * r;
            if (!m->deafened) {
                out[2 * k] += g * l;
                out[2 * k + 1] += g * r;
            }
        }
        v->head = (v->head + take) % FIFO_LEN;
        v->count -= take;
        if (take < MIXER_BLOCK)
            v->started = 0; /* ran dry: buffer again */
        if (energy / (2 * MIXER_BLOCK) > LOUD)
            v->level = 5;
        else if (v->level > 0)
            v->level--;
    }
    for (int k = 0; k < 2 * MIXER_BLOCK; k++)
        out[k] = out[k] > 1.f ? 1.f : out[k] < -1.f ? -1.f : out[k];
}

void mixer_remove(mixer_t *m, unsigned long long user)
{
    for (int i = 0; i < m->nvoices; i++)
        if (m->voices[i]->user == user) {
            mem_free(m->voices[i]);
            m->voices[i] = m->voices[--m->nvoices];
            return;
        }
}

void mixer_set_volume(mixer_t *m, unsigned long long user, float volume)
{
    mixer_voice_t *v = voice(m, user);

    v->volume = volume < 0 ? 0 : volume > 2 ? 2 : volume;
}

int mixer_speaking(const mixer_t *m, unsigned long long user)
{
    const mixer_voice_t *v = find(m, user);

    return v && v->level > 0;
}
