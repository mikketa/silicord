#include <windows.h>
#include <mmsystem.h>
#include "audio.h"

#define BLOCK 960  /* 20 ms at 48 kHz */
#define NBUF 4     /* 80 ms queued on each side */

typedef struct {
    audio_io_t io;
    HANDLE thread, stop, out_event, in_event;
    HWAVEOUT out;
    HWAVEIN in;
    WAVEHDR out_hdr[NBUF], in_hdr[NBUF];
    short out_buf[NBUF][BLOCK * 2], in_buf[NBUF][BLOCK];
    float mix[BLOCK * 2], mic[BLOCK];
} audio_t;

static audio_t g_audio;

static short to16(float v)
{
    v *= 32768.f;
    return (short)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)(v < 0 ? v - 0.5f : v + 0.5f));
}

static void play_block(audio_t *a, WAVEHDR *h)
{
    short *buf = (short *)h->lpData;

    a->io.play(a->io.ctx, a->mix);
    for (int i = 0; i < BLOCK * 2; i++)
        buf[i] = to16(a->mix[i]);
    waveOutWrite(a->out, h, sizeof *h);
}

static DWORD WINAPI audio_main(LPVOID arg)
{
    audio_t *a = arg;
    HANDLE events[3] = {a->stop, a->out_event, a->in_event};
    DWORD n = a->in ? 3 : 2;

    for (int i = 0; i < NBUF; i++)
        play_block(a, &a->out_hdr[i]);
    while (WaitForMultipleObjects(n, events, FALSE, INFINITE) != WAIT_OBJECT_0) {
        for (int i = 0; i < NBUF; i++)
            if (a->out_hdr[i].dwFlags & WHDR_DONE)
                play_block(a, &a->out_hdr[i]);
        for (int i = 0; a->in && i < NBUF; i++) {
            WAVEHDR *h = &a->in_hdr[i];
            if (!(h->dwFlags & WHDR_DONE))
                continue;
            if (h->dwBytesRecorded == sizeof a->in_buf[i]) {
                const short *s = (const short *)h->lpData;
                for (int k = 0; k < BLOCK; k++)
                    a->mic[k] = s[k] * (1.f / 32768.f);
                a->io.capture(a->io.ctx, a->mic);
            }
            h->dwFlags &= ~WHDR_DONE;
            waveInAddBuffer(a->in, h, sizeof *h);
        }
    }
    return 0;
}

static void open_input(audio_t *a)
{
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 1, 48000, 48000 * 2, 2, 16, 0};

    if (waveInOpen(&a->in, WAVE_MAPPER, &f, (DWORD_PTR)a->in_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        a->in = NULL;
        return;
    }
    for (int i = 0; i < NBUF; i++) {
        a->in_hdr[i].lpData = (LPSTR)a->in_buf[i];
        a->in_hdr[i].dwBufferLength = sizeof a->in_buf[i];
        waveInPrepareHeader(a->in, &a->in_hdr[i], sizeof a->in_hdr[i]);
        waveInAddBuffer(a->in, &a->in_hdr[i], sizeof a->in_hdr[i]);
    }
    waveInStart(a->in);
}

int audio_start(const audio_io_t *io)
{
    audio_t *a = &g_audio;
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};

    audio_stop();
    ZeroMemory(a, sizeof *a);
    a->io = *io;
    a->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    a->out_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    a->in_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&a->out, WAVE_MAPPER, &f, (DWORD_PTR)a->out_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        a->out = NULL;
        audio_stop();
        return 0;
    }
    for (int i = 0; i < NBUF; i++) {
        a->out_hdr[i].lpData = (LPSTR)a->out_buf[i];
        a->out_hdr[i].dwBufferLength = sizeof a->out_buf[i];
        waveOutPrepareHeader(a->out, &a->out_hdr[i], sizeof a->out_hdr[i]);
    }
    if (io->capture)
        open_input(a);
    a->thread = CreateThread(NULL, 0, audio_main, a, 0, NULL);
    return a->thread != NULL;
}

void audio_stop(void)
{
    audio_t *a = &g_audio;

    if (a->thread) {
        SetEvent(a->stop);
        WaitForSingleObject(a->thread, INFINITE);
        CloseHandle(a->thread);
        a->thread = NULL;
    }
    if (a->in) {
        waveInReset(a->in);
        for (int i = 0; i < NBUF; i++)
            waveInUnprepareHeader(a->in, &a->in_hdr[i], sizeof a->in_hdr[i]);
        waveInClose(a->in);
        a->in = NULL;
    }
    if (a->out) {
        waveOutReset(a->out);
        for (int i = 0; i < NBUF; i++)
            waveOutUnprepareHeader(a->out, &a->out_hdr[i], sizeof a->out_hdr[i]);
        waveOutClose(a->out);
        a->out = NULL;
    }
    if (a->stop) {
        CloseHandle(a->stop);
        CloseHandle(a->out_event);
        CloseHandle(a->in_event);
        a->stop = a->out_event = a->in_event = NULL;
    }
}
