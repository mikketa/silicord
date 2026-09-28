#pragma once
#include "json.h"
#include "sb.h"

/*
 * Discord gateway: Hello, Identify or Resume, Heartbeat, dispatches.
 * Callbacks run on the thread that called gw_run(); JSON slices are only
 * valid during the call.
 */
typedef struct {
    void *ctx;
    void (*status)(void *ctx, const char *text);
    void (*ready)(void *ctx, json_t d);
    /* Every other dispatch: t is the event name (a JSON string), d its payload. */
    void (*dispatch)(void *ctx, json_t t, json_t d);
} gw_events_t;

typedef enum {
    GW_STOPPED,     /* gw_stop() was called */
    GW_RESUME,      /* connection lost: reconnect and resume the session */
    GW_REIDENTIFY,  /* the session cannot be resumed: reconnect and identify again */
    GW_AUTH_FAILED, /* the token is no longer valid */
    GW_FATAL,       /* Discord refused the connection for good */
} gw_result_t;

/*
 * Runs one connection until it ends. With `resume`, picks up the previous
 * session (Discord replays missed events). `established` is set once READY or
 * RESUMED was received, so callers can reset their backoff.
 */
gw_result_t gw_run(const char *token, const gw_events_t *ev, int resume, int *established);
/* Sends a payload on the open connection (any thread). Returns 0 when not connected. */
int gw_send(const sb_t *payload);
/* The current session's id (empty before READY), for interactions. */
void gw_session_id(char *out, size_t size);
/* Waits up to `ms`; returns 1 if gw_stop() was called meanwhile. */
int gw_wait(unsigned ms);
/* Safe from any thread. Stays in effect, so later gw_run() calls return at once, until gw_reset(). */
void gw_stop(void);
/* Also forgets the session, so the next gw_run() identifies. */
void gw_reset(void);
