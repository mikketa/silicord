/* Message parsing and formatting tests. */
#include <windows.h>
#include "test.h"
#include "msg.h"

static int parse(const char *s, msg_t *m)
{
    json_t v;

    *m = (msg_t){0};
    return json_parse(s, sc_strlen(s), &v) && msg_parse(v, m);
}

static void expect_text(const char *json, const char *text, const char *name)
{
    msg_t m;

    check(parse(json, &m) && str_eq(&m.text, text), name);
    msg_free(&m);
}

static void test_content(void)
{
    expect_text("{\"id\":\"1\",\"content\":\"hi <@42> and <@!43>\",\"mentions\":["
                "{\"id\":\"42\",\"username\":\"bob\",\"global_name\":null},"
                "{\"id\":\"43\",\"username\":\"x\",\"global_name\":\"Ann\"}]}",
                "hi @bob and @Ann", "user mentions use the display name");
    expect_text("{\"id\":\"1\",\"content\":\"<:pog:123> <a:dance:456>\"}", ":pog: :dance:", "custom emoji");
    expect_text("{\"id\":\"1\",\"content\":\"<@&9> <@77>\",\"mentions\":[]}", "@role @unknown-user",
                "role and unknown mentions");
    expect_text("{\"id\":\"1\",\"content\":\"see <#5> <3 a<b\"}", "see <#5> <3 a<b",
                "channel mentions and stray brackets are kept");
    expect_text("{\"id\":\"1\",\"content\":\"\",\"attachments\":[{\"filename\":\"a.png\"}]}",
                "\xF0\x9F\x93\x8E a.png", "attachment only");
    expect_text("{\"id\":\"1\",\"content\":\"look\",\"attachments\":[{\"filename\":\"a.png\"}]}",
                "look\n\xF0\x9F\x93\x8E a.png", "text then attachment");
    expect_text("{\"id\":\"1\",\"type\":7,\"content\":\"\"}", "joined the server.", "join notice");
}

static void test_author_and_reply(void)
{
    msg_t m;

    check(parse("{\"id\":\"1\",\"author\":{\"id\":\"42\",\"username\":\"bob\",\"global_name\":\"Bob\",\"avatar\":\"abc\"},"
                "\"member\":{\"nick\":\"Bobby\"},\"content\":\"x\"}", &m) &&
          str_eq(&m.author, "Bobby") && lstrcmpA(m.author_id, "42") == 0 && lstrcmpA(m.avatar, "abc") == 0,
          "server nickname wins over the display name");
    msg_free(&m);

    check(parse("{\"id\":\"2\",\"type\":19,\"content\":\"yes\",\"referenced_message\":"
                "{\"author\":{\"username\":\"ann\"},\"content\":\"first line\\nsecond\"}}", &m) &&
          str_eq(&m.reply, "ann: first line") && str_eq(&m.text, "yes"),
          "reply keeps the first line of the original");
    msg_free(&m);

    check(parse("{\"id\":\"3\",\"content\":\"x\",\"referenced_message\":null}", &m) && m.reply.len == 0,
          "no reply when referenced_message is null");
    msg_free(&m);
}

static void test_batch(void)
{
    static const char arr[] = "[{\"id\":\"3\"},{\"id\":\"2\"},{\"nope\":1},{\"id\":\"1\"}]";
    json_t v;
    msg_batch_t *b;

    json_parse(arr, sizeof arr - 1, &v);
    b = msg_batch_from_array(v, BATCH_HISTORY, "99", 4);
    check(b->n == 3, "invalid items are skipped");
    check(b->n == 3 && lstrcmpA(b->msgs[0].id, "1") == 0 && lstrcmpA(b->msgs[2].id, "3") == 0,
          "batch is oldest first");
    check(b->has_more && lstrcmpA(b->channel_id, "99") == 0, "full page means more history");
    msg_batch_free(b);

    check(snowflake_ms("175928847299117063") == 1462015105796ll, "snowflake timestamp");
}

void entry(void)
{
    test_content();
    test_author_and_reply();
    test_batch();
    finish();
}
