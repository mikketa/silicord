/* Blocks a worker thread until the user finishes the captcha Discord just returned. */
#include <windows.h>
#include "solve.h"
#include "mem.h"
#include "ui.h"

enum { SOLVE_OK = 1, SOLVE_FAIL = 2 };

static CRITICAL_SECTION g_lock;
static HANDLE g_event;
static volatile LONG g_seq;
static volatile LONG g_kind;
static sb_t g_reply;
static int g_ready;

void solve_init(void)
{
    if (g_ready)
        return;
    InitializeCriticalSection(&g_lock);
    g_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_ready = 1;
}

static sb_t *payload(const props_captcha_t *c)
{
    sb_t *sb = mem_alloc(sizeof *sb);

    sb_add(sb, "{\"service\":");
    sb_json_str(sb, c->service.data ? c->service.data : "", c->service.len);
    sb_add(sb, ",\"sitekey\":");
    sb_json_str(sb, c->sitekey.data ? c->sitekey.data : "", c->sitekey.len);
    sb_add(sb, ",\"rqdata\":");
    sb_json_str(sb, c->rqdata.data ? c->rqdata.data : "", c->rqdata.len);
    sb_add(sb, ",\"invisible\":");
    sb_add(sb, c->invisible ? "true" : "false");
    sb_add(sb, "}");
    return sb;
}

static void reply(int kind, const char *text)
{
    solve_init();
    sb_free(&g_reply);
    sb_add(&g_reply, text ? text : "");
    InterlockedExchange(&g_kind, kind);
    InterlockedIncrement(&g_seq);
    SetEvent(g_event);
}

void solve_done(const char *token)
{
    reply(SOLVE_OK, token);
}

void solve_fail(const char *err)
{
    reply(SOLVE_FAIL, err && err[0] ? err : "The check was not completed.");
}

int solve_captcha(const props_captcha_t *challenge, sb_t *headers)
{
    LONG seen;
    int kind = 0;

    if (!challenge || !challenge->sitekey.len || ui_on_ui_thread() || !ui_window())
        return 0;
    solve_init();
    EnterCriticalSection(&g_lock);
    seen = g_seq;
    if (!ui_post(UI_CAPTCHA, payload(challenge))) {
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    for (;;) {
        if (g_seq != seen) {
            kind = (int)g_kind;
            if (kind == SOLVE_OK && g_reply.len)
                props_captcha_headers(headers, challenge, g_reply.data, g_reply.len);
            sb_free(&g_reply);
            break;
        }
        WaitForSingleObject(g_event, INFINITE);
        ResetEvent(g_event);
    }
    LeaveCriticalSection(&g_lock);
    return kind == SOLVE_OK;
}
