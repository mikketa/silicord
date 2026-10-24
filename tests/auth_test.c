/* Login API: request bodies and every kind of answer, with the JSON Discord sends. */
#include <windows.h>
#include "test.h"
#include "auth.h"

static auth_result_t g_r;

static int answer(unsigned status, const char *body)
{
    auth_read(body, body ? sc_strlen(body) : 0, status, &g_r);
    return g_r.kind;
}

static void test_answers(void)
{
    check(answer(200, "{\"user_id\":\"1\",\"token\":\"tok.abc\",\"user_settings\":{\"locale\":\"fr\",\"theme\":\"dark\"}}") ==
                  AUTH_TOKEN && str_eq(&g_r.token, "tok.abc"),
          "a token logs in");

    check(answer(200, "{\"user_id\":\"1\",\"mfa\":true,\"sms\":true,\"ticket\":\"T1\",\"backup\":true,\"totp\":true,"
                    "\"webauthn\":\"{\\\"publicKey\\\":{\\\"challenge\\\":\\\"AAECAw\\\",\\\"timeout\\\":60000,"
                    "\\\"rpId\\\":\\\"discord.com\\\",\\\"allowCredentials\\\":[{\\\"type\\\":\\\"public-key\\\","
                    "\\\"id\\\":\\\"BQYH\\\"}],\\\"userVerification\\\":\\\"required\\\",\\\"extensions\\\":{\\\"uvm\\\":true}}}\","
                    "\"login_instance_id\":\"L1\"}") == AUTH_MFA &&
              str_eq(&g_r.ticket, "T1") && str_eq(&g_r.instance, "L1") &&
              g_r.methods == (MFA_WEBAUTHN | MFA_TOTP | MFA_BACKUP | MFA_SMS),
          "two-factor: ticket, instance and every method");
    {
        auth_webauthn_t w = {0};
        check(auth_webauthn_read(g_r.webauthn.data, g_r.webauthn.len, &w) && bytes_eq(&w.challenge, "\x00\x01\x02\x03", 4) &&
                  str_eq(&w.rp_id, "discord.com") && w.timeout == 60000 && w.uv == 2 && w.ncreds == 1 &&
                  bytes_eq(&w.creds[0], "\x05\x06\x07", 3),
              "a security key's request options");
        auth_webauthn_free(&w);
    }
    check(answer(200, "{\"user_id\":\"1\",\"mfa\":true,\"sms\":false,\"ticket\":\"T2\",\"backup\":false,\"totp\":true,"
                    "\"webauthn\":null,\"login_instance_id\":\"L2\"}") == AUTH_MFA && g_r.methods == MFA_TOTP,
          "an authenticator app only");

    check(answer(400, "{\"code\":50035,\"errors\":{\"login\":{\"_errors\":[{\"code\":\"INVALID_LOGIN\",\"message\":"
                    "\"Login or password is invalid.\"}]},\"password\":{\"_errors\":[{\"code\":\"INVALID_LOGIN\","
                    "\"message\":\"Login or password is invalid.\"}]}},\"message\":\"Invalid Form Body\"}") == AUTH_FIELDS &&
              str_eq(&g_r.login_error, "Login or password is invalid.") &&
              str_eq(&g_r.password_error, "Login or password is invalid."),
          "a wrong password marks both fields");
    check(answer(400, "{\"login\":[\"Login or password is invalid.\"],\"password\":[\"Login or password is invalid.\"]}") ==
                  AUTH_FAILED,
          "flattened errors without a code are not taken for the form's");
    check(answer(400, "{\"code\":50035,\"login\":[\"Must be 6 or more in length.\"]}") == AUTH_FIELDS &&
              str_eq(&g_r.login_error, "Must be 6 or more in length.") && !g_r.password_error.len,
          "flattened field errors");
    check(answer(400, "{\"code\":50035,\"errors\":{\"login\":{\"_errors\":[{\"code\":\"ACCOUNT_LOGIN_VERIFICATION_EMAIL\","
                    "\"message\":\"New login location detected, please check your e-mail.\"}]}},"
                    "\"message\":\"Invalid Form Body\"}") == AUTH_FIELDS &&
              str_eq(&g_r.login_error, "New login location detected, please check your e-mail."),
          "a new location shows under the login");

    check(answer(429, "{\"message\":\"You are being rate limited.\",\"retry_after\":1.234,\"global\":false}") ==
                  AUTH_RATE_LIMITED && g_r.retry_after == 2,
          "rate limited, rounded up to whole seconds");
    check(answer(400, "{\"message\":\"Invalid two-factor code\",\"code\":60008}") == AUTH_FIELDS &&
              str_eq(&g_r.code_error, "Invalid two-factor code"),
          "a wrong code marks the code");
    check(answer(400, "{\"message\":\"Invalid two-factor auth ticket\",\"code\":60006}") == AUTH_EXPIRED, "expired ticket");
    check(answer(400, "{\"message\":\"This account is disabled\",\"code\":20013}") == AUTH_DISABLED, "disabled account");
    check(answer(400, "{\"message\":\"Scheduled\",\"code\":20011}") == AUTH_DISABLED, "account scheduled for deletion");
    check(answer(400, "{\"message\":\"New login location\",\"code\":70007}") == AUTH_NEW_LOCATION && g_r.message.len,
          "new location confirmed by SMS");
    check(answer(403, "{\"user_id\":\"1\",\"suspended_user_token\":\"x\"}") == AUTH_SUSPENDED, "suspended account");
    check(answer(0, NULL) == AUTH_FAILED && g_r.message.len, "Discord out of reach");
    check(answer(400, "{\"captcha_key\":[\"captcha-required\"],\"captcha_sitekey\":\"k\",\"captcha_service\":\"hcaptcha\"}") ==
                  AUTH_FAILED && str_eq(&g_r.message, "Complete the captcha to log in."),
          "a captcha left unsolved");
    check(answer(502, "<html>Bad gateway</html>") == AUTH_FAILED && g_r.message.len, "an error page");

    check(answer(200, "{\"phone\":\"+*******0085\"}") == AUTH_OK && str_eq(&g_r.phone, "+*******0085"), "SMS sent");
    check(answer(200, "{\"method\":\"password_reset\"}") == AUTH_OK && str_eq(&g_r.method, "password_reset"),
          "password reset sent");
    check(answer(204, "") == AUTH_OK, "no content");

    check(answer(200, "{\"ticket\":\"PT\",\"challenge\":\"{\\\"publicKey\\\":{\\\"challenge\\\":\\\"_-8\\\","
                    "\\\"timeout\\\":60000,\\\"rpId\\\":\\\"discord.com\\\",\\\"allowCredentials\\\":[],"
                    "\\\"userVerification\\\":\\\"preferred\\\"}}\"}") == AUTH_CHALLENGE &&
              str_eq(&g_r.ticket, "PT"),
          "passkey challenge");
    {
        auth_webauthn_t w = {0};
        check(auth_webauthn_read(g_r.webauthn.data, g_r.webauthn.len, &w) && bytes_eq(&w.challenge, "\xFF\xEF", 2) &&
                  w.ncreds == 0 && w.uv == 1,
              "a passkey's request options: base64url, no credential list");
        auth_webauthn_free(&w);
        check(!auth_webauthn_read("{\"publicKey\":{\"timeout\":1}}", 27, &w), "options without a challenge");
        auth_webauthn_free(&w);
    }
    auth_result_free(&g_r);
}

static void test_bodies(void)
{
    sb_t b = {0};

    auth_login_body(&b, "me@example.com", 14, "a\"b\\c", 5);
    check(str_eq(&b, "{\"login\":\"me@example.com\",\"password\":\"a\\\"b\\\\c\",\"undelete\":false,"
                     "\"login_source\":null,\"gift_code_sku_id\":null}"),
          "login body, password escaped");
    sb_clear(&b);
    auth_mfa_body(&b, "123 456", 7, "T1", "L1");
    check(str_eq(&b, "{\"code\":\"123456\",\"ticket\":\"T1\",\"login_source\":null,\"gift_code_sku_id\":null,"
                     "\"login_instance_id\":\"L1\"}"),
          "MFA body, spaces dropped");
    sb_clear(&b);
    auth_mfa_body(&b, "abcd-efgh", 9, "T1", NULL);
    check(str_eq(&b, "{\"code\":\"abcdefgh\",\"ticket\":\"T1\",\"login_source\":null,\"gift_code_sku_id\":null,"
                     "\"login_instance_id\":null}"),
          "backup code, dash dropped, no instance");
    sb_clear(&b);
    auth_mfa_body(&b, "{\"id\":\"a-b\"}", 12, "T1", "L1");
    check(str_eq(&b, "{\"code\":\"{\\\"id\\\":\\\"a-b\\\"}\",\"ticket\":\"T1\",\"login_source\":null,"
                     "\"gift_code_sku_id\":null,\"login_instance_id\":\"L1\"}"),
          "an assertion goes as a string, untouched");
    check(lstrcmpA(auth_mfa_path(MFA_BACKUP), "/auth/mfa/backup") == 0 &&
              lstrcmpA(auth_mfa_path(MFA_TOTP), "/auth/mfa/totp") == 0 &&
              lstrcmpA(auth_mfa_path(MFA_SMS), "/auth/mfa/sms") == 0 &&
              lstrcmpA(auth_mfa_path(MFA_WEBAUTHN), "/auth/mfa/webauthn") == 0,
          "MFA endpoints");
    sb_clear(&b);
    auth_ticket_body(&b, "T1");
    check(str_eq(&b, "{\"ticket\":\"T1\"}"), "SMS send body");
    sb_clear(&b);
    auth_forgot_body(&b, "+33612345678", 12);
    check(str_eq(&b, "{\"login\":\"+33612345678\"}"), "forgot password body");
    sb_clear(&b);
    auth_finish_body(&b, "{\"x\":1}", 7, "PT");
    check(str_eq(&b, "{\"credential\":\"{\\\"x\\\":1}\",\"ticket\":\"PT\",\"source\":null,\"giftCodeSKUId\":null}"),
          "passkey finish body");
    sb_free(&b);
}

static void test_webauthn(void)
{
    static const unsigned char challenge[] = {0, 1, 2, 3}, id[] = {1, 2, 3, 4, 5}, data[] = {0xAA, 0xAA, 0xAA, 0xAA},
                               sig[] = {0x30, 0x45}, user[] = {9, 8, 7};
    sb_t cd = {0}, cred = {0};
    auth_assertion_t a = {0};

    auth_client_data(&cd, challenge, sizeof challenge);
    check(str_eq(&cd, "{\"type\":\"webauthn.get\",\"challenge\":\"AAECAw\",\"origin\":\"https://discord.com\","
                      "\"crossOrigin\":false}"),
          "clientDataJSON as a browser on discord.com writes it");

    a.id = id;
    a.id_n = sizeof id;
    a.auth_data = data;
    a.auth_data_n = sizeof data;
    a.client_data = (const unsigned char *)"{}";
    a.client_data_n = 2;
    a.signature = sig;
    a.signature_n = sizeof sig;
    a.user_handle = user;
    a.user_handle_n = sizeof user;
    a.attachment = "platform";
    auth_credential(&cred, &a);
    check(str_eq(&cred, "{\"authenticatorAttachment\":\"platform\",\"clientExtensionResults\":{},\"id\":\"AQIDBAU\","
                        "\"rawId\":\"AQIDBAU\",\"type\":\"public-key\",\"response\":{\"authenticatorData\":\"qqqqqg\","
                        "\"clientDataJSON\":\"e30\",\"signature\":\"MEU\",\"userHandle\":\"CQgH\"}}"),
          "credential JSON, base64url without padding");
    sb_clear(&cred);
    a.user_handle_n = 0;
    a.attachment = NULL;
    auth_credential(&cred, &a);
    check(str_eq(&cred, "{\"authenticatorAttachment\":null,\"clientExtensionResults\":{},\"id\":\"AQIDBAU\","
                        "\"rawId\":\"AQIDBAU\",\"type\":\"public-key\",\"response\":{\"authenticatorData\":\"qqqqqg\","
                        "\"clientDataJSON\":\"e30\",\"signature\":\"MEU\",\"userHandle\":null}}"),
          "a security key's assertion has no user handle");
    sb_free(&cd);
    sb_free(&cred);
}

void entry(void)
{
    test_answers();
    test_bodies();
    test_webauthn();
    finish();
}
