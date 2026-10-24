/* The startup chime: a valid WAV, silent before the first note, loud enough, never clipping, ending on silence. */
#include "test.h"
#include "chime.h"
#include "mem.h"

static int sample(const unsigned char *wav, int i)
{
    return (short)(wav[44 + 2 * i] | wav[45 + 2 * i] << 8);
}

static unsigned u32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

void entry(void)
{
    static const int onsets[8] = {100, 300, 400, 550, 650, 800, 1000, 1100};
    size_t size;
    unsigned char *wav = chime_wav(onsets, 8, 1500, 2400, &size);
    int n = 2400 * CHIME_RATE / 1000, peak = 0, quiet = 1, sounds = 0;

    check(size == 44 + (size_t)n * 2, "one 16-bit sample per tick");
    check(u32(wav) == 0x46464952 && u32(wav + 8) == 0x45564157 && u32(wav + 12) == 0x20746D66, "RIFF WAVE fmt");
    check(u32(wav + 4) == size - 8 && u32(wav + 40) == (unsigned)n * 2 && u32(wav + 36) == 0x61746164, "chunk sizes");
    check(u32(wav + 24) == CHIME_RATE && (wav[22] | wav[23] << 8) == 1 && (wav[34] | wav[35] << 8) == 16,
          "mono, 16 bits, at the chime's rate");
    for (int i = 0; i < 100 * CHIME_RATE / 1000; i++)
        quiet &= sample(wav, i) == 0;
    check(quiet, "silent before the first note");
    for (int i = 110 * CHIME_RATE / 1000; i < 130 * CHIME_RATE / 1000; i++)
        sounds |= sample(wav, i) != 0;
    check(sounds, "the first note sounds");
    for (int i = 0; i < n; i++) {
        int v = sample(wav, i) < 0 ? -sample(wav, i) : sample(wav, i);
        peak = v > peak ? v : peak;
    }
    check(peak > 32767 / 20 && peak < 32767 / 2, "between -26 and -6 dB at its loudest: soft, never clipping");
    check(sample(wav, n - 1) == 0, "fades out to silence");
    mem_free(wav);
    finish();
}
