#pragma once
#include "json.h"

/*
 * Discord gateway: Hello, Identify, Heartbeat, Ready. Callbacks run on the
 * thread that called gw_run(); JSON slices are only valid during the call.
 */
typedef struct {
    void *ctx;
    void (*status)(void *ctx, const char *text);
    void (*ready)(void *ctx, json_t d);
} gw_events_t;

/* Blocks until the connection closes or gw_stop() is called. */
int gw_run(const char *token, const gw_events_t *ev);
/* Safe from any thread. */
void gw_stop(void);
