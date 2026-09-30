#pragma once
#include "props.h"

/* One captcha dialog shared by every Discord request. Call from a worker thread, never the UI thread. */
void solve_init(void);
/* Shows the challenge and, on success, appends the X-Captcha-* headers. Returns 0 if the user cancels or the view fails. */
int solve_captcha(const props_captcha_t *challenge, sb_t *headers);
/* UI thread. */
void solve_done(const char *token);
void solve_fail(const char *err);
