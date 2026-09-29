#pragma once

/*
 * What Windows plays, except our own sound (the call we would send back to
 * it): WASAPI process loopback, which excludes a process tree, from Windows
 * 10 version 2004. A capture thread hands on 20 ms mono blocks at 48 kHz,
 * silent ones while nothing plays, so the stream's timing never breaks.
 */

/* A block of 960 samples in [-1, 1], on the capture thread. */
typedef void (*loopback_block_fn)(void *ctx, const float *pcm);

/* Returns 0 when Windows cannot capture sound this way. */
int loopback_start(loopback_block_fn block, void *ctx);
void loopback_stop(void);
