#pragma once
#include <stddef.h>

/*
 * A voice connection (voice gateway v8): the WebSocket for signalling and
 * DAVE, UDP for RTP. One at a time; callbacks run on the voice threads.
 */

enum { VOICE_OFF, VOICE_CONNECTING, VOICE_CONNECTED, VOICE_FAILED };

typedef struct {
    unsigned long long server_id; /* the guild, or the channel for calls */
    unsigned long long channel_id;
    unsigned long long user_id;
    char session_id[80];
    char token[128];
    char endpoint[256]; /* host[:port], from VOICE_SERVER_UPDATE */
} voice_params_t;

typedef struct {
    void *ctx;
    void (*state)(void *ctx, int state, const char *text);
    /* One decrypted Opus packet from `user`, with its RTP sequence number. */
    void (*frame)(void *ctx, unsigned long long user, unsigned seq, const unsigned char *opus, size_t n);
    /* Someone left the call. */
    void (*left)(void *ctx, unsigned long long user);
    /* Someone's camera or stream started or stopped. */
    void (*video_state)(void *ctx, unsigned long long user, int on);
    /* One decrypted VP8 frame of `user`'s video. */
    void (*video)(void *ctx, unsigned long long user, const unsigned char *vp8, size_t n);
    /* Someone watching our video lost it: the next frame should be a key frame. */
    void (*key_frame)(void *ctx);
    /* A line for the --debug trace (opcodes, DAVE states, the Opus modes heard). */
    void (*log)(void *ctx, const char *text);
} voice_events_t;

int voice_start(const voice_params_t *p, const voice_events_t *ev);
/* Leaves: closes the WebSocket and waits for the threads. */
void voice_stop(void);
/* Sends one 20 ms Opus frame. */
int voice_send(const unsigned char *opus, size_t n);
/* Ends a burst of speech: silence frames, then not speaking. */
void voice_quiet(void);
/* Starts or stops sending our video (announced to the server first). */
int voice_video_active(int on);
/* One VP8 frame of our video; `timestamp` counts at 90 kHz. */
int voice_video_send(const unsigned char *vp8, size_t n, unsigned timestamp);
/* The call's end-to-end encryption code (30 digits); 0 when media is not end-to-end encrypted. */
int voice_privacy_code(char *out, size_t size);
