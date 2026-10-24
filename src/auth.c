#include "auth.h"
#include "b64.h"
#include "json.h"
#include "sc_asm.h"

/* Discord's error codes on the login endpoints. */
#define ERR_FORM 50035
#define ERR_DELETION 20011
#define ERR_DISABLED 20013
#define ERR_TICKET 60006
#define ERR_CODE 60008
#define ERR_PHONE_LOCATION 70007
#define ERR_EMAIL_LOCATION 70009

void auth_result_free(auth_result_t *r)
{
    sb_free(&r->token);
    sb_free(&r->ticket);
    sb_free(&r->instance);
    sb_free(&r->webauthn);
    sb_free(&r->login_error);
    sb_free(&r->password_error);
    sb_free(&r->code_error);
    sb_free(&r->message);
    sb_free(&r->phone);
    sb_free(&r->method);
    r->kind = AUTH_FAILED;
    r->methods = 0;
    r->retry_after = 0;
}

static int take_str(json_t obj, const char *key, sb_t *out)
{
    json_t v;

    return json_get(obj, key, &v) && json_type(v) == JSON_STRING && json_str(v, out);
}

static int is_true(json_t obj, const char *key)
{
    json_t v;

    return json_get(obj, key, &v) && json_type(v) == JSON_TRUE;
}

/*
 * A field's first error: {"errors": {"login": {"_errors": [{"message": ...}]}}},
 * or flattened the way older answers had it, {"login": ["..."]}.
 */
static int field_error(json_t root, const char *field, sb_t *out)
{
    json_t errors, f, list, first, msg;
    json_iter_t it;

    if (json_get(root, "errors", &errors) && json_get(errors, field, &f) && json_get(f, "_errors", &list)) {
        json_iter(list, &it);
        if (json_next(&it, NULL, &first) && json_get(first, "message", &msg) && json_type(msg) == JSON_STRING)
            return json_str(msg, out);
    }
    if (json_get(root, field, &list) && json_type(list) == JSON_ARRAY) {
        json_iter(list, &it);
        if (json_next(&it, NULL, &first) && json_type(first) == JSON_STRING)
            return json_str(first, out);
    }
    return 0;
}

/* retry_after is a float ("1.5"): whole seconds, rounded up, at least one. */
static int seconds_up(json_t v)
{
    const char *p = v.p;
    int s = 0, frac = 0;

    if (json_type(v) != JSON_NUMBER || *p == '-')
        return 1;
    for (; p < v.end && *p >= '0' && *p <= '9' && s < 86400; p++)
        s = s * 10 + (*p - '0');
    if (p < v.end && *p == '.')
        for (p++; p < v.end && *p >= '0' && *p <= '9'; p++)
            frac |= *p != '0';
    s += frac;
    return s > 0 ? s : 1;
}

static void set_message(auth_result_t *r, const char *text)
{
    sb_clear(&r->message);
    sb_add(&r->message, text);
}

static void read_success(json_t root, auth_result_t *r)
{
    json_t v;

    if (take_str(root, "token", &r->token)) {
        r->kind = AUTH_TOKEN;
        return;
    }
    if (is_true(root, "mfa")) {
        r->kind = AUTH_MFA;
        take_str(root, "ticket", &r->ticket);
        take_str(root, "login_instance_id", &r->instance);
        /* `webauthn` is null without a security key or passkey, else its request options as a string. */
        if (take_str(root, "webauthn", &r->webauthn) && r->webauthn.len)
            r->methods |= MFA_WEBAUTHN;
        if (is_true(root, "totp"))
            r->methods |= MFA_TOTP;
        if (is_true(root, "backup"))
            r->methods |= MFA_BACKUP;
        if (is_true(root, "sms"))
            r->methods |= MFA_SMS;
        return;
    }
    if (take_str(root, "ticket", &r->ticket) && json_get(root, "challenge", &v)) {
        r->kind = AUTH_CHALLENGE;
        if (json_type(v) == JSON_STRING)
            json_str(v, &r->webauthn);
        else
            sb_addn(&r->webauthn, v.p, (size_t)(v.end - v.p));
        return;
    }
    take_str(root, "phone", &r->phone);
    take_str(root, "method", &r->method);
    r->kind = AUTH_OK;
}

void auth_read(const char *body, size_t n, unsigned status, auth_result_t *r)
{
    json_t root = {0}, v;
    long long code = 0;
    int parsed;

    auth_result_free(r);
    if (!status) {
        set_message(r, "Could not reach Discord. Check your connection and try again.");
        return;
    }
    parsed = body && n && json_parse(body, n, &root) && json_type(root) == JSON_OBJECT;
    if (status == 429) {
        r->kind = AUTH_RATE_LIMITED;
        r->retry_after = parsed && json_get(root, "retry_after", &v) ? seconds_up(v) : 5;
        set_message(r, "You are being rate limited. Wait a moment and try again.");
        return;
    }
    if (status >= 200 && status < 300) {
        r->kind = AUTH_OK;
        if (parsed)
            read_success(root, r);
        return;
    }
    if (parsed) {
        if (json_get(root, "code", &v))
            json_int(v, &code);
        take_str(root, "message", &r->message);
    }
    if (status == 403 && parsed && json_get(root, "suspended_user_token", &v)) {
        r->kind = AUTH_SUSPENDED;
        set_message(r, "This account has been suspended. Log in on discord.com to see why.");
        return;
    }
    switch (code) {
    case ERR_FORM: {
        if (!field_error(root, "login", &r->login_error))
            field_error(root, "email", &r->login_error);
        field_error(root, "password", &r->password_error);
        field_error(root, "code", &r->code_error);
        if (r->login_error.len || r->password_error.len || r->code_error.len)
            r->kind = AUTH_FIELDS;
        break;
    }
    case ERR_CODE:
        r->kind = AUTH_FIELDS;
        sb_add(&r->code_error, r->message.len ? r->message.data : "Invalid two-factor code");
        break;
    case ERR_TICKET:
        r->kind = AUTH_EXPIRED;
        set_message(r, "This login attempt expired. Log in again.");
        break;
    case ERR_DELETION:
    case ERR_DISABLED:
        r->kind = AUTH_DISABLED;
        set_message(r, code == ERR_DISABLED ? "This account is disabled. Log in on discord.com to restore it."
                                            : "This account is scheduled for deletion. Log in on discord.com to restore it.");
        break;
    case ERR_PHONE_LOCATION:
    case ERR_EMAIL_LOCATION:
        r->kind = AUTH_NEW_LOCATION;
        set_message(r, code == ERR_PHONE_LOCATION
                           ? "New login location detected. Log in once on discord.com to confirm it with the code we sent by SMS."
                           : "New login location detected, please check your e-mail.");
        break;
    default:
        break;
    }
    if (r->kind == AUTH_FAILED && parsed && json_get(root, "captcha_key", &v))
        set_message(r, "Complete the captcha to log in.");
    if (r->kind == AUTH_FAILED && !r->message.len)
        set_message(r, "Something went wrong. Try again.");
}

/* ---- Requests ---- */

void auth_login_body(sb_t *out, const char *login, size_t ln, const char *password, size_t pn)
{
    sb_add(out, "{\"login\":");
    sb_json_str(out, login, ln);
    sb_add(out, ",\"password\":");
    sb_json_str(out, password, pn);
    sb_add(out, ",\"undelete\":false,\"login_source\":null,\"gift_code_sku_id\":null}");
}

const char *auth_mfa_path(unsigned method)
{
    switch (method) {
    case MFA_WEBAUTHN: return "/auth/mfa/webauthn";
    case MFA_BACKUP:   return "/auth/mfa/backup";
    case MFA_SMS:      return "/auth/mfa/sms";
    default:           return "/auth/mfa/totp";
    }
}

static void add_opt_str(sb_t *out, const char *s)
{
    if (s && *s)
        sb_json_str(out, s, sc_strlen(s));
    else
        sb_add(out, "null");
}

void auth_mfa_body(sb_t *out, const char *code, size_t cn, const char *ticket, const char *instance)
{
    sb_t clean = {0};

    /* "123 456" and "abcd-efgh" as typed; an assertion is JSON and keeps everything. */
    if (cn && code[0] == '{') {
        sb_addn(&clean, code, cn);
    } else {
        for (size_t i = 0; i < cn; i++)
            if (code[i] != ' ' && code[i] != '-')
                sb_addn(&clean, &code[i], 1);
    }
    sb_add(out, "{\"code\":");
    sb_json_str(out, clean.data ? clean.data : "", clean.len);
    sb_add(out, ",\"ticket\":");
    add_opt_str(out, ticket);
    sb_add(out, ",\"login_source\":null,\"gift_code_sku_id\":null,\"login_instance_id\":");
    add_opt_str(out, instance);
    sb_add(out, "}");
    sb_free(&clean);
}

void auth_ticket_body(sb_t *out, const char *ticket)
{
    sb_add(out, "{\"ticket\":");
    add_opt_str(out, ticket);
    sb_add(out, "}");
}

void auth_forgot_body(sb_t *out, const char *login, size_t ln)
{
    sb_add(out, "{\"login\":");
    sb_json_str(out, login, ln);
    sb_add(out, "}");
}

void auth_finish_body(sb_t *out, const char *credential, size_t n, const char *ticket)
{
    /* The website spells these two in camel case here. */
    sb_add(out, "{\"credential\":");
    sb_json_str(out, credential, n);
    sb_add(out, ",\"ticket\":");
    add_opt_str(out, ticket);
    sb_add(out, ",\"source\":null,\"giftCodeSKUId\":null}");
}

/* ---- WebAuthn ---- */

void auth_webauthn_free(auth_webauthn_t *w)
{
    sb_free(&w->challenge);
    sb_free(&w->rp_id);
    for (int i = 0; i < w->ncreds; i++)
        sb_free(&w->creds[i]);
    w->ncreds = 0;
    w->timeout = 0;
    w->uv = 0;
}

static int b64_field(json_t obj, const char *key, sb_t *out)
{
    sb_t s = {0};
    int ok = take_str(obj, key, &s) && s.len && b64_decode(s.data, s.len, out);

    sb_free(&s);
    return ok;
}

int auth_webauthn_read(const char *json, size_t n, auth_webauthn_t *w)
{
    json_t root, pk, v, list, item;
    json_iter_t it;
    long long t;
    sb_t uv = {0};

    auth_webauthn_free(w);
    if (!json || !json_parse(json, n, &root) || json_type(root) != JSON_OBJECT)
        return 0;
    if (!json_get(root, "publicKey", &pk))
        pk = root;
    if (!b64_field(pk, "challenge", &w->challenge))
        return 0;
    if (!take_str(pk, "rpId", &w->rp_id) || !w->rp_id.len) {
        sb_clear(&w->rp_id);
        sb_add(&w->rp_id, "discord.com");
    }
    w->timeout = json_get(pk, "timeout", &v) && json_int(v, &t) && t > 0 && t < 600000 ? (unsigned)t : 60000;
    take_str(pk, "userVerification", &uv);
    w->uv = uv.len == 8 && uv.data[0] == 'r' ? 2 : uv.len == 11 && uv.data[0] == 'd' ? 0 : 1;
    sb_free(&uv);
    if (json_get(pk, "allowCredentials", &list)) {
        json_iter(list, &it);
        while (w->ncreds < AUTH_MAX_CREDS && json_next(&it, NULL, &item))
            if (b64_field(item, "id", &w->creds[w->ncreds]))
                w->ncreds++;
            else
                sb_free(&w->creds[w->ncreds]);
    }
    return 1;
}

void auth_client_data(sb_t *out, const unsigned char *challenge, size_t n)
{
    sb_add(out, "{\"type\":\"webauthn.get\",\"challenge\":\"");
    b64url_encode(challenge, n, out);
    sb_add(out, "\",\"origin\":\"https://discord.com\",\"crossOrigin\":false}");
}

static void add_b64url(sb_t *out, const char *key, const unsigned char *p, size_t n)
{
    sb_add(out, "\"");
    sb_add(out, key);
    sb_add(out, "\":\"");
    b64url_encode(p, n, out);
    sb_add(out, "\"");
}

void auth_credential(sb_t *out, const auth_assertion_t *a)
{
    sb_add(out, "{\"authenticatorAttachment\":");
    add_opt_str(out, a->attachment);
    sb_add(out, ",\"clientExtensionResults\":{},");
    add_b64url(out, "id", a->id, a->id_n);
    sb_add(out, ",");
    add_b64url(out, "rawId", a->id, a->id_n);
    sb_add(out, ",\"type\":\"public-key\",\"response\":{");
    add_b64url(out, "authenticatorData", a->auth_data, a->auth_data_n);
    sb_add(out, ",");
    add_b64url(out, "clientDataJSON", a->client_data, a->client_data_n);
    sb_add(out, ",");
    add_b64url(out, "signature", a->signature, a->signature_n);
    sb_add(out, ",");
    if (a->user_handle_n)
        add_b64url(out, "userHandle", a->user_handle, a->user_handle_n);
    else
        sb_add(out, "\"userHandle\":null");
    sb_add(out, "}}");
}
