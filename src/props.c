#include "props.h"
#include "b64.h"
#include "json.h"
#include "sc_asm.h"

unsigned props_find_build(const char *s, size_t n)
{
    static const char key[] = "\"BUILD_NUMBER\":";
    size_t klen = sizeof key - 1;

    if (!s)
        return 0;
    for (size_t i = 0; i + klen < n; i++) {
        size_t j = 0, p;
        unsigned v = 0;
        int digits = 0;

        while (j < klen && s[i + j] == key[j])
            j++;
        if (j != klen)
            continue;
        p = i + klen;
        if (p < n && (s[p] == ' ' || s[p] == '\t'))
            p++;
        if (p < n && s[p] == '"')
            p++;
        if (p >= n || s[p] < '0' || s[p] > '9')
            continue;
        while (p < n && s[p] >= '0' && s[p] <= '9' && digits < 9) {
            v = v * 10u + (unsigned)(s[p] - '0');
            p++;
            digits++;
        }
        if (digits)
            return v;
    }
    return 0;
}

static void add_str(sb_t *out, const char *s)
{
    size_t n = s ? sc_strlen(s) : 0;

    sb_json_str(out, s ? s : "", n);
}

void props_json(sb_t *out, const props_in_t *in)
{
    const props_in_t none = {0};
    const props_in_t *p = in ? in : &none;

    sb_add(out, "{\"os\":\"Windows\",\"browser\":\"Chrome\",\"device\":\"\",\"system_locale\":");
    add_str(out, p->locale && p->locale[0] ? p->locale : "en-US");
    sb_add(out, ",\"has_client_mods\":false,\"browser_user_agent\":");
    add_str(out, p->ua);
    sb_add(out, ",\"browser_version\":");
    add_str(out, p->browser_version);
    sb_add(out, ",\"os_version\":\"10\",\"referrer\":\"\",\"referring_domain\":\"\","
                "\"referrer_current\":\"\",\"referring_domain_current\":\"\","
                "\"release_channel\":\"stable\",\"client_build_number\":");
    sb_u64(out, p->build);
    sb_add(out, ",\"client_event_source\":null,\"client_launch_id\":");
    add_str(out, p->launch_id);
    sb_add(out, ",\"client_heartbeat_session_id\":");
    add_str(out, p->heartbeat_id);
    sb_add(out, "}");
}

void props_b64(sb_t *out, const props_in_t *in)
{
    sb_t json = {0};

    props_json(&json, in);
    b64_encode((const unsigned char *)json.data, json.len, out);
    sb_free(&json);
}

static int header_safe(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\r' || s[i] == '\n' || s[i] == '\0')
            return 0;
    return 1;
}

static void add_hdr(sb_t *out, const char *name, const char *value, size_t n)
{
    if (!n || !header_safe(value, n))
        return;
    sb_add(out, name);
    sb_add(out, ": ");
    sb_addn(out, value, n);
    sb_add(out, "\r\n");
}

static int take_str(json_t obj, const char *key, sb_t *out)
{
    json_t v;

    return json_get(obj, key, &v) && json_type(v) == JSON_STRING && json_str(v, out);
}

static int captcha_key_is(json_t root, const char *name)
{
    json_t key, item;
    json_iter_t it;

    if (!json_get(root, "captcha_key", &key) || json_type(key) != JSON_ARRAY)
        return 0;
    json_iter(key, &it);
    while (json_next(&it, NULL, &item))
        if (json_str_eq(item, name))
            return 1;
    return 0;
}

void props_captcha_clear(props_captcha_t *c)
{
    sb_free(&c->service);
    sb_free(&c->sitekey);
    sb_free(&c->rqdata);
    sb_free(&c->rqtoken);
    sb_free(&c->session);
    c->present = 0;
    c->invisible = 0;
}

int props_captcha_read(const char *json, size_t n, props_captcha_t *c)
{
    json_t root, invis;

    props_captcha_clear(c);
    if (!json || !json_parse(json, n, &root) || json_type(root) != JSON_OBJECT)
        return 0;
    take_str(root, "captcha_sitekey", &c->sitekey);
    if (!c->sitekey.len && !captcha_key_is(root, "captcha-required"))
        return 0;
    c->present = 1;
    if (!take_str(root, "captcha_service", &c->service))
        sb_add(&c->service, "hcaptcha");
    take_str(root, "captcha_rqdata", &c->rqdata);
    take_str(root, "captcha_rqtoken", &c->rqtoken);
    take_str(root, "captcha_session_id", &c->session);
    c->invisible = json_get(root, "should_serve_invisible", &invis) && json_type(invis) == JSON_TRUE;
    return 1;
}

void props_captcha_headers(sb_t *out, const props_captcha_t *c, const char *solution, size_t n)
{
    add_hdr(out, "X-Captcha-Key", solution, n);
    if (c) {
        add_hdr(out, "X-Captcha-Rqtoken", c->rqtoken.data, c->rqtoken.len);
        add_hdr(out, "X-Captcha-Session-Id", c->session.data, c->session.len);
    }
}

int props_wants_captcha(const char *json, size_t n)
{
    props_captcha_t c = {0};
    int yes = props_captcha_read(json, n, &c);

    props_captcha_clear(&c);
    return yes;
}
