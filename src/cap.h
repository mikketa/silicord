#pragma once
#include <windows.h>

/*
 * Shows one captcha inside `parent`. The widget is loaded from discord.com:
 * the site key is locked to that host, so a local page cannot display it.
 * `done` runs on the UI thread. A synchronous failure returns 0 and does not
 * call `done`; a failure after the view has started calls `done` with an error.
 */
typedef void (*cap_done_fn)(void *ctx, const char *token, const char *error);

typedef struct {
    const char *service; /* hcaptcha, recaptcha, recaptcha_enterprise */
    const char *sitekey;
    const char *rqdata; /* may be empty; required when Discord sent one */
    int invisible;
} cap_req_t;

int cap_open(HWND parent, RECT bounds, int dpi, const cap_req_t *req, cap_done_fn done, void *ctx);
void cap_bounds(RECT bounds, int dpi);
void cap_close(void);
