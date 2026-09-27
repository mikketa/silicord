/* JSON reader tests. Same no-CRT setup as the client; exit code = number of failures. */
#include <windows.h>
#include "json.h"
#include "sb.h"
#include "sc_asm.h"

static int g_failed;

static void out(const char *s)
{
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)sc_strlen(s), &w, NULL);
}

static void check(int cond, const char *name)
{
    out(cond ? "ok   " : "FAIL ");
    out(name);
    out("\r\n");
    if (!cond)
        g_failed++;
}

static int parse(const char *s, json_t *v)
{
    return json_parse(s, sc_strlen(s), v);
}

static int bytes_eq(const sb_t *sb, const char *s)
{
    size_t n = sc_strlen(s);

    if (sb->len != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (sb->data[i] != s[i])
            return 0;
    return 1;
}

static void test_parse(void)
{
    json_t v;

    check(parse(" {\"a\": [1, 2, {\"b\": null}], \"c\": true} ", &v), "parse nested document");
    check(json_type(v) == JSON_OBJECT, "root is an object");
    check(parse("[]", &v) && json_count(v) == 0, "empty array");
    check(parse("{}", &v) && json_count(v) == 0, "empty object");
    check(!parse("{\"a\": 1,}", &v), "reject trailing comma");
    check(!parse("{\"a\" 1}", &v), "reject missing colon");
    check(!parse("[1, 2", &v), "reject unterminated array");
    check(!parse("\"abc", &v), "reject unterminated string");
    check(!parse("1 2", &v), "reject trailing data");
    check(!parse("", &v), "reject empty input");
}

static void test_get(void)
{
    json_t root, d, v;
    long long n;

    parse("{\"op\": 10, \"d\": {\"heartbeat_interval\": 41250, \"x\": -7}, \"t\": \"READY\"}", &root);
    check(json_get(root, "op", &v) && json_int(v, &n) && n == 10, "get int");
    check(json_get(root, "d", &d) && json_type(d) == JSON_OBJECT, "get object");
    check(json_get(d, "heartbeat_interval", &v) && json_int(v, &n) && n == 41250, "get nested int");
    check(json_get(d, "x", &v) && json_int(v, &n) && n == -7, "negative int");
    check(!json_get(root, "missing", &v), "missing key");
    check(!json_get(root, "o", &v), "key prefix is not a match");
    check(json_get(root, "t", &v) && json_str_eq(v, "READY"), "string equals");
    check(!json_str_eq(v, "READ"), "string prefix is not equal");
}

static void test_iter(void)
{
    json_t root, key, val;
    json_iter_t it;
    long long sum = 0, n;

    parse("[1, 2, 3, 4]", &root);
    check(json_count(root) == 4, "array count");
    json_iter(root, &it);
    while (json_next(&it, NULL, &val))
        if (json_int(val, &n))
            sum += n;
    check(sum == 10, "array iteration");

    parse("{\"a\": 1, \"b\": [2, 3]}", &root);
    json_iter(root, &it);
    check(json_next(&it, &key, &val) && json_str_eq(key, "a"), "object iteration key 1");
    check(json_next(&it, &key, &val) && json_str_eq(key, "b") && json_count(val) == 2, "object iteration key 2");
    check(!json_next(&it, &key, &val), "object iteration end");
}

static void test_str(void)
{
    json_t v;
    sb_t s = {0};

    parse("\"line\\nbreak \\\"quoted\\\" \\\\ \\/\"", &v);
    check(json_str(v, &s) && bytes_eq(&s, "line\nbreak \"quoted\" \\ /"), "simple escapes");
    sb_clear(&s);

    parse("\"caf\\u00e9\"", &v);
    check(json_str(v, &s) && bytes_eq(&s, "caf\xC3\xA9"), "\\u escape to utf-8");
    sb_clear(&s);

    parse("\"\\ud83d\\ude00\"", &v);
    check(json_str(v, &s) && bytes_eq(&s, "\xF0\x9F\x98\x80"), "surrogate pair to utf-8");
    sb_clear(&s);

    parse("\"d\xC3\xA9j\xC3\xA0\"", &v);
    check(json_str(v, &s) && bytes_eq(&s, "d\xC3\xA9j\xC3\xA0"), "raw utf-8 passthrough");
    sb_free(&s);

    sb_json_str(&s, "a\"b\\c\n\x01", 7);
    check(bytes_eq(&s, "\"a\\\"b\\\\c\\n\\u0001\""), "encode string");
    sb_free(&s);
}

void entry(void)
{
    test_parse();
    test_get();
    test_iter();
    test_str();
    out(g_failed ? "\r\nsome tests failed\r\n" : "\r\nall tests passed\r\n");
    ExitProcess((UINT)g_failed);
}
