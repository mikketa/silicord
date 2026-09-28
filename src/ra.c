#include <windows.h>
#include "ra.h"
#include "b64.h"
#include "crypto.h"
#include "http.h"
#include "json.h"
#include "ws.h"

#define RA_HOST L"remote-auth-gateway.discord.gg"
#define RA_PATH L"/?v=2"
/* The gateway rejects upgrades that do not come from the Discord origin. */
#define RA_HEADERS L"Origin: https://discord.com"
#define QR_URL_PREFIX "https://discord.com/ra/"

enum { STOP, CONTINUE, DONE };

typedef struct {
    ws_t ws;
    HANDLE cancel;     /* set by ra_cancel() until ra_reset() */
    HANDLE done;       /* set when one ra_login() run ends */
    HANDLE heartbeat;
    const ra_events_t *ev;
    rsa_key_t *key;
    sb_t *token;
    DWORD interval;
    volatile LONG acked;
    volatile LONG ready;
} ra_t;

static ra_t g_ra;

static void status(ra_t *r, const char *text)
{
    if (r->ev->status)
        r->ev->status(r->ev->ctx, text);
}

static int send_op(ra_t *r, const char *op, const char *key, const char *value, size_t value_len)
{
    sb_t msg = {0};
    int ok;

    sb_add(&msg, "{\"op\":\"");
    sb_add(&msg, op);
    sb_add(&msg, "\"");
    if (key) {
        sb_add(&msg, ",\"");
        sb_add(&msg, key);
        sb_add(&msg, "\":");
        sb_json_str(&msg, value, value_len);
    }
    sb_add(&msg, "}");
    ok = ws_send(&r->ws, &msg);
    sb_free(&msg);
    return ok;
}

static DWORD WINAPI heartbeat_main(LPVOID arg)
{
    ra_t *r = arg;
    HANDLE events[2] = {r->done, r->cancel};

    while (WaitForMultipleObjects(2, events, FALSE, r->interval) == WAIT_TIMEOUT) {
        if (!InterlockedExchange(&r->acked, 0) || !send_op(r, "heartbeat", NULL, NULL, 0)) {
            ra_cancel();
            break;
        }
    }
    return 0;
}

/* Base64-decodes a string field and decrypts it with our private key. */
static int decrypt_field(ra_t *r, json_t obj, const char *key, sb_t *out)
{
    json_t v;
    sb_t b64 = {0}, raw = {0};
    int ok;

    ok = json_get(obj, key, &v) && json_str(v, &b64) &&
         b64_decode(b64.data, b64.len, &raw) &&
         rsa_decrypt_oaep_sha256(r->key, raw.data, raw.len, out);
    sb_free(&raw);
    sb_free(&b64);
    return ok;
}

static int send_init(ra_t *r)
{
    sb_t der = {0}, b64 = {0};
    int ok;

    ok = rsa_public_spki(r->key, &der);
    b64_encode((const unsigned char *)der.data, der.len, &b64);
    ok = ok && send_op(r, "init", "encoded_public_key", b64.data, b64.len);
    sb_free(&b64);
    sb_free(&der);
    return ok;
}

static int send_proof(ra_t *r, json_t root)
{
    sb_t nonce = {0}, proof = {0};
    unsigned char hash[32];
    int ok = 0;

    if (decrypt_field(r, root, "encrypted_nonce", &nonce)) {
        sha256(nonce.data, nonce.len, hash);
        b64url_encode(hash, sizeof hash, &proof);
        ok = send_op(r, "nonce_proof", "proof", proof.data, proof.len);
    }
    sb_free(&proof);
    sb_free(&nonce);
    return ok;
}

static void show_qr(ra_t *r, json_t root)
{
    json_t fp;
    sb_t url = {0};

    if (json_get(root, "fingerprint", &fp)) {
        sb_add(&url, QR_URL_PREFIX);
        json_str(fp, &url);
        if (r->ev->qr)
            r->ev->qr(r->ev->ctx, url.data);
    }
    sb_free(&url);
}

/* The payload is "id:discriminator:avatar:username". */
static void show_scanned(ra_t *r, json_t root)
{
    sb_t payload = {0};
    const char *name;
    int colons = 0;

    if (decrypt_field(r, root, "encrypted_user_payload", &payload)) {
        name = payload.data;
        for (size_t i = 0; i < payload.len && colons < 3; i++)
            if (payload.data[i] == ':') {
                colons++;
                name = payload.data + i + 1;
            }
        if (r->ev->scanned)
            r->ev->scanned(r->ev->ctx, colons == 3 ? name : "");
    }
    sb_free(&payload);
}

/* Trades the ticket for the encrypted token. */
static int exchange_ticket(ra_t *r, json_t root)
{
    json_t ticket, root2, v;
    sb_t body = {0};
    http_resp_t resp = {0};
    int ok = 0;

    if (!json_get(root, "ticket", &ticket))
        return 0;
    sb_add(&body, "{\"ticket\":");
    sb_addn(&body, ticket.p, (size_t)(ticket.end - ticket.p));
    sb_add(&body, "}");

    if (!http_request("POST", "/users/@me/remote-auth/login", NULL, body.data, body.len, &resp)) {
        status(r, "Could not reach discord.com");
    } else if (resp.status == 200 && json_parse(resp.body.data, resp.body.len, &root2) &&
               decrypt_field(r, root2, "encrypted_token", r->token)) {
        ok = 1;
    } else if (json_parse(resp.body.data, resp.body.len, &root2) && json_get(root2, "captcha_key", &v)) {
        status(r, "Discord asked for a captcha. Log in with a token instead.");
    } else {
        sb_t text = {0};
        sb_add(&text, "Login failed (HTTP ");
        sb_u64(&text, resp.status);
        sb_add(&text, ")");
        status(r, text.data);
        sb_free(&text);
    }
    http_resp_free(&resp);
    sb_free(&body);
    return ok;
}

static int handle(ra_t *r, const sb_t *msg)
{
    json_t root, op, v;
    long long interval;

    if (!json_parse(msg->data, msg->len, &root) || !json_get(root, "op", &op))
        return CONTINUE;

    if (json_str_eq(op, "hello")) {
        if (!json_get(root, "heartbeat_interval", &v) || !json_int(v, &interval) || interval <= 0)
            return STOP;
        r->interval = (DWORD)interval;
        r->acked = 1;
        r->heartbeat = CreateThread(NULL, 0, heartbeat_main, r, 0, NULL);
        return send_init(r) ? CONTINUE : STOP;
    }
    if (json_str_eq(op, "heartbeat_ack")) {
        InterlockedExchange(&r->acked, 1);
        return CONTINUE;
    }
    if (json_str_eq(op, "nonce_proof"))
        return send_proof(r, root) ? CONTINUE : STOP;
    if (json_str_eq(op, "pending_remote_init")) {
        show_qr(r, root);
        return CONTINUE;
    }
    if (json_str_eq(op, "pending_ticket")) {
        show_scanned(r, root);
        return CONTINUE;
    }
    if (json_str_eq(op, "pending_login"))
        return exchange_ticket(r, root) ? DONE : STOP;
    if (json_str_eq(op, "cancel")) {
        status(r, "Login cancelled on the phone");
        return STOP;
    }
    return CONTINUE;
}

/* Lazily creates the lock and events; ra_login() and ra_reset() run on one thread. */
static void ra_reset_once(ra_t *r)
{
    if (r->ready)
        return;
    ws_init(&r->ws);
    r->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    r->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&r->ready, 1);
}

static int cancelled(ra_t *r)
{
    return WaitForSingleObject(r->cancel, 0) == WAIT_OBJECT_0;
}

int ra_login(const ra_events_t *ev, sb_t *token)
{
    ra_t *r = &g_ra;
    sb_t msg = {0};
    int result = STOP;

    ra_reset_once(r);
    if (cancelled(r))
        return 0;
    ResetEvent(r->done);
    r->ev = ev;
    r->token = token;
    r->heartbeat = NULL;
    r->key = rsa_generate();
    if (!r->key) {
        status(r, "Could not generate a key pair");
        return 0;
    }

    if (!ws_connect(&r->ws, RA_HOST, RA_PATH, RA_HEADERS)) {
        status(r, "Could not reach the login server");
    } else if (!cancelled(r)) {
        while (ws_recv(&r->ws, &msg) && (result = handle(r, &msg)) == CONTINUE)
            ;
        if (result == CONTINUE && !cancelled(r))
            status(r, "The QR code expired");
    }

    SetEvent(r->done);
    if (r->heartbeat) {
        WaitForSingleObject(r->heartbeat, INFINITE);
        CloseHandle(r->heartbeat);
    }
    ws_shutdown(&r->ws, WS_CLOSE_NORMAL);
    ws_close(&r->ws);
    rsa_free(r->key);
    r->key = NULL;
    sb_free(&msg);
    return result == DONE;
}

void ra_cancel(void)
{
    ra_t *r = &g_ra;

    if (!r->ready)
        return;
    SetEvent(r->cancel);
    ws_shutdown(&r->ws, WS_CLOSE_NORMAL);
}

void ra_reset(void)
{
    ra_reset_once(&g_ra);
    ResetEvent(g_ra.cancel);
}
