#pragma once
#include <stddef.h>
#include "sb.h"

/*
 * Discord's web client properties: the JSON object sent as gateway identify
 * `properties` and, base64-encoded, as the X-Super-Properties header.
 * A captcha is something the user solves. This module only recognizes the
 * challenge and builds the headers that carry the solution back.
 */

typedef struct {
    const char *ua;
    const char *browser_version; /* "148.0.0.0", the same token as in `ua` */
    const char *locale;          /* "fr-FR" */
    const char *launch_id;       /* UUID, one per process */
    const char *heartbeat_id;    /* UUID, one per process */
    unsigned build;              /* client_build_number from discord.com */
} props_in_t;

/* 0 when the page does not carry "BUILD_NUMBER". */
unsigned props_find_build(const char *s, size_t n);

void props_json(sb_t *out, const props_in_t *in);
void props_b64(sb_t *out, const props_in_t *in);

typedef struct {
    int present;
    int invisible;
    sb_t service; /* "hcaptcha", "recaptcha", "recaptcha_enterprise" */
    sb_t sitekey;
    sb_t rqdata;
    sb_t rqtoken;
    sb_t session;
} props_captcha_t;

void props_captcha_clear(props_captcha_t *c);
/* 1 when `json` is a Discord captcha challenge. */
int props_captcha_read(const char *json, size_t n, props_captcha_t *c);
/* X-Captcha-* lines. A solution with a line break is dropped. */
void props_captcha_headers(sb_t *out, const props_captcha_t *c, const char *solution, size_t n);
/* 1 when the body is a captcha challenge. */
int props_wants_captcha(const char *json, size_t n);
