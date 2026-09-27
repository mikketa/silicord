#pragma once
#include "sb.h"

/*
 * QR code login ("remote auth"): the Discord mobile app scans a code and the
 * user confirms on the phone, so passkeys, 2FA and SMS all happen there.
 * Callbacks run on the thread that called ra_login().
 */
typedef struct {
    void *ctx;
    void (*qr)(void *ctx, const char *url);        /* show this URL as a QR code */
    void (*scanned)(void *ctx, const char *name);  /* waiting for confirmation on the phone */
    void (*status)(void *ctx, const char *text);
} ra_events_t;

/*
 * Blocks until the user confirms on the phone (returns 1 and appends the token),
 * cancels, or the code expires after about two minutes (returns 0).
 */
int ra_login(const ra_events_t *ev, sb_t *token);
/* Safe from any thread. Stays in effect, so later ra_login() calls return at once, until ra_reset(). */
void ra_cancel(void);
void ra_reset(void);
