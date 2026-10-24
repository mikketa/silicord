#pragma once
#include <stddef.h>
#include "sb.h"

/*
 * Discord's login API as the website uses it (/api/v9/auth/...): the request
 * bodies, and what each answer means. No I/O here: main.c sends, the UI shows.
 * Captchas never reach this module, the HTTP layer solves them and retries.
 * Passwords, codes and tickets are wiped when the buffers holding them are freed.
 */

/* What an answer from the login endpoints means. */
enum {
    AUTH_FAILED,       /* `message` says why */
    AUTH_TOKEN,        /* logged in: `token` */
    AUTH_MFA,          /* a second factor is needed: `ticket`, `instance`, `methods`, `webauthn` */
    AUTH_CHALLENGE,    /* passkey login started: `ticket` and `webauthn` */
    AUTH_OK,           /* done, nothing to read but `phone` (SMS sent) or `method` (password reset sent) */
    AUTH_FIELDS,       /* the form is wrong: `login_error`, `password_error` or `code_error` */
    AUTH_EXPIRED,      /* the MFA or passkey ticket ran out: start again */
    AUTH_DISABLED,     /* the account is disabled or scheduled for deletion */
    AUTH_NEW_LOCATION, /* Discord wants this location confirmed (an SMS or an e-mail was sent) */
    AUTH_SUSPENDED,    /* the account is suspended */
    AUTH_RATE_LIMITED, /* too many tries: wait `retry_after` seconds */
};

/* Second factors, in the order the website offers them. */
enum { MFA_WEBAUTHN = 1, MFA_TOTP = 2, MFA_BACKUP = 4, MFA_SMS = 8 };

typedef struct {
    int kind;
    sb_t token;
    sb_t ticket;
    sb_t instance;          /* login_instance_id, echoed in the MFA request */
    unsigned methods;       /* MFA_* */
    sb_t webauthn;          /* the WebAuthn request options, as JSON text */
    sb_t login_error, password_error, code_error;
    sb_t message;           /* a sentence to show when nothing more precise applies */
    sb_t phone;             /* SMS sent to this masked number */
    sb_t method;            /* forgot password: "password_reset" or "one_time_login" */
    int retry_after;        /* seconds */
} auth_result_t;

void auth_result_free(auth_result_t *r);

/* Reads the answer of any login endpoint. `status` is the HTTP status, 0 when Discord could not be reached. */
void auth_read(const char *body, size_t n, unsigned status, auth_result_t *r);

/* POST /auth/login. */
void auth_login_body(sb_t *out, const char *login, size_t ln, const char *password, size_t pn);
/* POST /auth/mfa/{totp,backup,sms,webauthn}: a code as typed (spaces and dashes are dropped) or the assertion JSON. */
void auth_mfa_body(sb_t *out, const char *code, size_t cn, const char *ticket, const char *instance);
/* The endpoint for an MFA_* method, after /api/v9. */
const char *auth_mfa_path(unsigned method);
/* POST /auth/mfa/sms/send. */
void auth_ticket_body(sb_t *out, const char *ticket);
/* POST /auth/forgot. */
void auth_forgot_body(sb_t *out, const char *login, size_t ln);
/* POST /auth/conditional/finish, with the assertion JSON from auth_credential(). */
void auth_finish_body(sb_t *out, const char *credential, size_t n, const char *ticket);

/* ---- WebAuthn ---- */

#define AUTH_MAX_CREDS 16

typedef struct {
    sb_t challenge;         /* bytes */
    sb_t rp_id;
    unsigned timeout;       /* ms */
    int uv;                 /* 0 discouraged, 1 preferred, 2 required */
    int ncreds;             /* allowCredentials: empty for a passkey, which the authenticator finds itself */
    sb_t creds[AUTH_MAX_CREDS];
} auth_webauthn_t;

/* Reads request options (`webauthn` above: {"publicKey": {...}}, or the publicKey object itself). */
int auth_webauthn_read(const char *json, size_t n, auth_webauthn_t *w);
void auth_webauthn_free(auth_webauthn_t *w);
/* The clientDataJSON a browser on discord.com signs for this challenge. */
void auth_client_data(sb_t *out, const unsigned char *challenge, size_t n);

/* An assertion from the authenticator, every field as bytes. */
typedef struct {
    const unsigned char *id, *auth_data, *client_data, *signature, *user_handle;
    size_t id_n, auth_data_n, client_data_n, signature_n, user_handle_n;
    const char *attachment; /* "platform", "cross-platform" or NULL */
} auth_assertion_t;

/* The credential JSON the website sends: PublicKeyCredential.toJSON(), base64url without padding. */
void auth_credential(sb_t *out, const auth_assertion_t *a);
