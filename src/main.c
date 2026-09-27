/* Silicord - native Discord client for Windows. No C runtime: entry point is `entry`. */
#include <windows.h>
#include "console.h"
#include "cred.h"
#include "gw.h"
#include "http.h"
#include "json.h"
#include "sb.h"

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

static int cmd_login(void)
{
    sb_t token = {0};
    int ok = 0;

    con_print("Paste your Discord token (input is hidden): ");
    if (!con_read_secret(&token))
        con_print("no token entered\r\n");
    else if (check_token(token.data)) {
        ok = cred_save(token.data, token.len);
        con_print(ok ? "token saved in the Windows Credential Manager\r\n"
                     : "could not save the token\r\n");
    }
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

static BOOL WINAPI on_ctrl(DWORD type)
{
    (void)type;
    gw_stop();
    return TRUE;
}

static int cmd_run(void)
{
    sb_t token = {0};
    int ok = 0;

    if (!cred_load(&token)) {
        con_print("no saved token, run: silicord login\r\n");
        return 0;
    }
    if (check_token(token.data)) {
        SetConsoleCtrlHandler(on_ctrl, TRUE);
        ok = gw_run(token.data);
    }
    sb_free(&token);
    return ok;
}

/* Returns the first argument after the program name, or "" if there is none. */
static const char *first_arg(void)
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
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static int arg_is(const char *arg, const char *word)
{
    while (*word && *arg == *word) {
        arg++;
        word++;
    }
    return !*word && (!*arg || *arg == ' ' || *arg == '\t');
}

void entry(void)
{
    const char *arg = first_arg();
    int ok;

    con_init();
    if (!http_init()) {
        print_error("could not initialize WinHTTP", GetLastError());
        ExitProcess(1);
    }

    if (arg_is(arg, "login")) {
        ok = cmd_login();
    } else if (arg_is(arg, "logout")) {
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
