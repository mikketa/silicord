/* Web client properties and captcha header parsing. Exit code = number of failures. */
#include <windows.h>
#include "test.h"
#include "b64.h"
#include "props.h"

static void fill(props_in_t *in)
{
    in->ua = "Mozilla/5.0";
    in->browser_version = "148.0.0.0";
    in->locale = "fr-FR";
    in->launch_id = "11111111-1111-1111-1111-111111111111";
    in->heartbeat_id = "22222222-2222-2222-2222-222222222222";
    in->build = 623968;
}

static void test_props(void)
{
    props_in_t in = {0};
    sb_t json = {0}, b64 = {0}, raw = {0};
    const char *html = "\"NODE_ENV\":\"production\",\"BUILD_NUMBER\":\"623968\",\"RELEASE_CHANNEL\":\"stable\"";
    const char *bare = "\"BUILD_NUMBER\": 624001,";
    const char *expect = "{\"os\":\"Windows\",\"browser\":\"Chrome\",\"device\":\"\",\"system_locale\":\"fr-FR\","
                         "\"has_client_mods\":false,\"browser_user_agent\":\"Mozilla/5.0\","
                         "\"browser_version\":\"148.0.0.0\",\"os_version\":\"10\",\"referrer\":\"\","
                         "\"referring_domain\":\"\",\"referrer_current\":\"\",\"referring_domain_current\":\"\","
                         "\"release_channel\":\"stable\",\"client_build_number\":623968,\"client_event_source\":null,"
                         "\"client_launch_id\":\"11111111-1111-1111-1111-111111111111\","
                         "\"client_heartbeat_session_id\":\"22222222-2222-2222-2222-222222222222\"}";

    check(props_find_build(html, sc_strlen(html)) == 623968, "quoted build number");
    check(props_find_build(bare, sc_strlen(bare)) == 624001, "bare build number");
    check(props_find_build("nope", 4) == 0, "missing build number");

    fill(&in);
    props_json(&json, &in);
    check(str_eq(&json, expect), "web client properties");
    props_b64(&b64, &in);
    check(b64_decode(b64.data, b64.len, &raw) && bytes_eq(&raw, json.data, json.len), "properties base64");
    sb_free(&json);
    sb_free(&b64);
    sb_free(&raw);

    in.ua = "a\"b";
    props_json(&json, &in);
    check(json.len > 10 && json.data[0] == '{', "escaped user agent still json");
    {
        size_t i = 0;
        int found = 0;
        const char *esc = "a\\\"b";
        size_t n = sc_strlen(esc);
        while (i + n <= json.len) {
            size_t j = 0;
            while (j < n && json.data[i + j] == esc[j])
                j++;
            if (j == n) {
                found = 1;
                break;
            }
            i++;
        }
        check(found, "quote in the user agent is escaped");
    }
    sb_free(&json);
}

static void test_captcha(void)
{
    props_captcha_t c = {0};
    sb_t hdr = {0};
    const char *cap = "{\"captcha_key\":[\"captcha-required\"],\"captcha_sitekey\":\"site\","
                      "\"captcha_service\":\"hcaptcha\",\"captcha_session_id\":\"sess\","
                      "\"captcha_rqdata\":\"rq+data/x\",\"captcha_rqtoken\":\"rqtok\","
                      "\"should_serve_invisible\":true}";
    const char *plain = "{\"message\":\"Invalid Form Body\",\"code\":50035}";

    check(props_captcha_read(cap, sc_strlen(cap), &c) && c.invisible && str_eq(&c.service, "hcaptcha") &&
              str_eq(&c.sitekey, "site") && str_eq(&c.rqdata, "rq+data/x") && str_eq(&c.rqtoken, "rqtok") &&
              str_eq(&c.session, "sess"),
          "captcha response");
    props_captcha_headers(&hdr, &c, "sol", 3);
    check(str_eq(&hdr, "X-Captcha-Key: sol\r\nX-Captcha-Rqtoken: rqtok\r\nX-Captcha-Session-Id: sess\r\n"),
          "captcha headers");
    sb_free(&hdr);
    props_captcha_headers(&hdr, &c, "bad\r\n", 5);
    check(str_eq(&hdr, "X-Captcha-Rqtoken: rqtok\r\nX-Captcha-Session-Id: sess\r\n"), "reject a header line break");
    sb_free(&hdr);
    props_captcha_clear(&c);

    check(!props_wants_captcha(plain, sc_strlen(plain)), "an ordinary error is not a captcha");
    check(props_wants_captcha(cap, sc_strlen(cap)), "captcha body is detected");
    check(!props_wants_captcha("[{\"content\":\"captcha_key\"}]", 28), "a message list is not a captcha");
}

void entry(void)
{
    test_props();
    test_captcha();
    finish();
}
