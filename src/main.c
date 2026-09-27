/* Silicord - native Discord client for Windows. No C runtime: entry point is `entry`. */
#include <windows.h>
#include "console.h"
#include "cred.h"
#include "gw.h"
#include "http.h"
#include "json.h"
#include "sb.h"

static volatile LONG g_cancelled;

static void print_error(const char *what, DWORD err)
{
    sb_t out = {0};

    sb_add(&out, what);
    sb_add(&out, " (error ");
    sb_u64(&out, err);
    sb_add(&out, ")\r\n");
    con_print_sb(&out);
    sb_free(&out);
}

static void print_line(const char *prefix, const char *text)
{
    sb_t out = {0};

    sb_add(&out, prefix);
    sb_add(&out, text);
    sb_add(&out, "\r\n");
    con_print_sb(&out);
    sb_free(&out);
}

/* GET /users/@me: prints the account name, returns 0 if the token is rejected. */
static int check_token(const char *token)
{
    http_resp_t resp = {0};
    json_t root, name;
    int ok = 0;

    if (!http_request("GET", "/users/@me", token, NULL, 0, &resp)) {
        print_error("could not reach discord.com", GetLastError());
    } else if (resp.status == 401) {
        con_print("the token was rejected by Discord\r\n");
    } else if (resp.status != 200) {
        sb_t out = {0};
        sb_add(&out, "unexpected HTTP status ");
        sb_u64(&out, resp.status);
        sb_add(&out, "\r\n");
        con_print_sb(&out);
        sb_free(&out);
    } else if (json_parse(resp.body.data, resp.body.len, &root) &&
               json_get(root, "username", &name)) {
        sb_t out = {0};
        sb_add(&out, "logged in as ");
        json_str(name, &out);
        sb_add(&out, "\r\n");
        con_print_sb(&out);
        sb_free(&out);
        ok = 1;
    }
    http_resp_free(&resp);
    return ok;
}

static int save_token(const sb_t *token)
{
    int ok = check_token(token->data) && cred_save(token->data, token->len);

    if (ok)
        con_print("token saved in the Windows Credential Manager\r\n");
    return ok;
}

static int login_token(void)
{
    sb_t token = {0};
    int ok = 0;

    con_print("Paste your Discord token (input is hidden): ");
    if (!con_read_secret(&token))
        con_print("no token entered\r\n");
    else
        ok = save_token(&token);
    sb_free(&token);
    return ok;
}

static int cmd_logout(void)
{
    if (!cred_delete()) {
        con_print("no saved token\r\n");
        return 0;
    }
    con_print("token removed\r\n");
    return 1;
}

static void on_gw_status(void *ctx, const char *text)
{
    (void)ctx;
    print_line("gateway: ", text);
}

static void on_ready(void *ctx, json_t d)
{
    json_t user, name, guilds;
    sb_t out = {0};

    (void)ctx;
    sb_add(&out, "gateway: ready");
    if (json_get(d, "user", &user) && json_get(user, "username", &name)) {
        sb_add(&out, " as ");
        json_str(name, &out);
    }
    if (json_get(d, "guilds", &guilds)) {
        sb_add(&out, ", ");
        sb_u64(&out, json_count(guilds));
        sb_add(&out, " servers");
    }
    sb_add(&out, "\r\n");
    con_print_sb(&out);
    sb_free(&out);
}

static int cmd_run(void)
{
    static const gw_events_t ev = {NULL, on_gw_status, on_ready};
    sb_t token = {0};
    int ok = 0;

    if (!cred_load(&token)) {
        con_print("no saved token, run: silicord login\r\n");
        return 0;
    }
    if (check_token(token.data))
        ok = gw_run(token.data, &ev);
    sb_free(&token);
    return ok;
}

static BOOL WINAPI on_ctrl(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_cancelled, 1);
    gw_stop();
    return TRUE;
}

static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* Returns the arguments after the program name. */
static const char *args(void)
{
    const char *p = GetCommandLineA();

    if (*p == '"') {
        for (p++; *p && *p != '"'; p++)
            ;
        if (*p)
            p++;
    } else {
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    return skip_space(p);
}

/* If `*p` starts with `word`, consumes it and returns 1. */
static int take(const char **p, const char *word)
{
    const char *s = *p;

    while (*word && *s == *word) {
        s++;
        word++;
    }
    if (*word || (*s && *s != ' ' && *s != '\t'))
        return 0;
    *p = skip_space(s);
    return 1;
}

void entry(void)
{
    const char *arg = args();
    int ok;

    con_init();
    if (!http_init()) {
        print_error("could not initialize WinHTTP", GetLastError());
        ExitProcess(1);
    }
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    if (take(&arg, "login")) {
        ok = login_token();
    } else if (take(&arg, "logout")) {
        ok = cmd_logout();
    } else if (!*arg) {
        con_print("\r\n");
        con_banner();
        con_print("\r\n");
        ok = cmd_run();
    } else {
        con_print("usage: silicord [login | logout]\r\n");
        ok = 0;
    }
    ExitProcess(ok ? 0 : 1);
}
