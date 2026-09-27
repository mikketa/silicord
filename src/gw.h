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
    /* Every other dispatch: t is the event name (a JSON string), d its payload. */
    void (*dispatch)(void *ctx, json_t t, json_t d);
} gw_events_t;

/* Blocks until the connection closes or gw_stop() is called. */
int gw_run(const char *token, const gw_events_t *ev);
/* Safe from any thread. Stays in effect, so later gw_run() calls return at once, until gw_reset(). */
void gw_stop(void);
void gw_reset(void);
