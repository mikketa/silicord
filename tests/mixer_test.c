/* The receive mixer: jitter buffering, concealment of gaps, silence and speaking detection. */
#include "test.h"
#include "mixer.h"

/* Opus silence as the SFU sends it: one 20 ms CELT frame. */
static const unsigned char k_silence[3] = {0xF8, 0xFF, 0xFE};

static int all_zero(const float *pcm)
{
    for (int i = 0; i < 2 * MIXER_BLOCK; i++)
        if (pcm[i] != 0)
            return 0;
    return 1;
}

void entry(void)
{
    static mixer_t m;
    static float out[2 * MIXER_BLOCK];

    mixer_init(&m);
    mixer_pull(&m, out);
    check(all_zero(out), "nothing to play");

    mixer_push(&m, 7, 100, k_silence, sizeof k_silence);
    check(m.nvoices == 1, "a speaker appears with its first packet");
    mixer_pull(&m, out);
    check(all_zero(out), "one block is not enough to start");
    mixer_push(&m, 7, 101, k_silence, sizeof k_silence);
    mixer_push(&m, 7, 101, k_silence, sizeof k_silence); /* duplicate */
    mixer_push(&m, 7, 99, k_silence, sizeof k_silence);  /* late */
    mixer_pull(&m, out);
    check(all_zero(out) && !mixer_speaking(&m, 7), "silence stays silent");
    mixer_push(&m, 7, 104, k_silence, sizeof k_silence); /* two lost packets are concealed */
    mixer_pull(&m, out);
    mixer_pull(&m, out);
    check(all_zero(out), "concealed silence");
    for (unsigned s = 105; s < 130; s++)
        mixer_push(&m, 7, s, k_silence, sizeof k_silence);
    check(m.nvoices == 1, "the queue stays bounded");
    mixer_set_volume(&m, 7, 5.f);
    mixer_push(&m, 8, 1, k_silence, sizeof k_silence);
    check(m.nvoices == 2, "two speakers");
    mixer_remove(&m, 7);
    check(m.nvoices == 1 && !mixer_speaking(&m, 7), "a speaker leaves");
    mixer_free(&m);
    finish();
}
