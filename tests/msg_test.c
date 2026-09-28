/* Message parsing and formatting tests. */
#include <windows.h>
#include "test.h"
#include "md.h"
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
                "hi " MD_MENTION_OPEN "@bob" MD_MENTION_CLOSE " and " MD_MENTION_OPEN "@Ann" MD_MENTION_CLOSE,
                "user mentions use the display name");
    expect_text("{\"id\":\"1\",\"content\":\"<:pog:123> <a:dance:456>\"}",
                MD_EMOJI_OPEN "123:pog" MD_EMOJI_CLOSE " " MD_EMOJI_OPEN "a456:dance" MD_EMOJI_CLOSE, "custom emoji");
    expect_text("{\"id\":\"1\",\"content\":\"<@&9> <@77>\",\"mentions\":[]}",
                MD_MENTION_OPEN "@role" MD_MENTION_CLOSE " " MD_MENTION_OPEN "@unknown-user" MD_MENTION_CLOSE,
                "role and unknown mentions");
    expect_text("{\"id\":\"1\",\"content\":\"see <#5> <3 a<b\"}", "see <#5> <3 a<b",
                "channel mentions and stray brackets are kept");
    expect_text("{\"id\":\"1\",\"content\":\"look\",\"attachments\":[{\"filename\":\"a.png\"}]}", "look",
                "attachments stay out of the text");
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
                "{\"author\":{\"username\":\"ann\"},\"content\":\"first line\\nsecond\"},"
                "\"message_reference\":{\"message_id\":\"1\"}}", &m) && lstrcmpA(m.reply_id, "1") == 0 &&
          str_eq(&m.reply, "ann: first line") && str_eq(&m.text, "yes"),
          "reply keeps the first line of the original");
    msg_free(&m);

    check(parse("{\"id\":\"3\",\"content\":\"x\",\"referenced_message\":null}", &m) && m.reply.len == 0,
          "no reply when referenced_message is null");
    msg_free(&m);
}

static void test_parts(void)
{
    msg_t m;

    check(parse("{\"id\":\"1\",\"content\":\"\",\"edited_timestamp\":\"2026-01-01T00:00:00\",\"attachments\":["
                "{\"filename\":\"cat.png\",\"size\":2048,\"width\":800,\"height\":600,\"content_type\":\"image/png\","
                "\"url\":\"https://cdn.discordapp.com/attachments/1/2/cat.png?ex=1\","
                "\"proxy_url\":\"https://media.discordapp.net/attachments/1/2/cat.png?ex=1\"},"
                "{\"filename\":\"SPOILER_notes.txt\",\"size\":10,\"content_type\":\"text/plain\","
                "\"url\":\"https://cdn.discordapp.com/attachments/1/3/SPOILER_notes.txt\"}]}", &m) &&
              !m.system && m.nfiles == 2,
          "attachments are kept, the message is not a system one");
    check(m.nfiles == 2 && m.files[0].image && m.files[0].width == 800 && m.files[0].size == 2048 &&
              str_eq(&m.files[0].url, "https://media.discordapp.net/attachments/1/2/cat.png?ex=1"),
          "images use the media proxy");
    check(m.nfiles == 2 && !m.files[1].image && m.files[1].spoiler &&
              str_eq(&m.files[1].url, "https://cdn.discordapp.com/attachments/1/3/SPOILER_notes.txt"),
          "other files keep their URL, spoilers are flagged");
    check(m.edited, "edited flag");
    msg_free(&m);

    check(parse("{\"id\":\"2\",\"content\":\"https://x.y\",\"embeds\":["
                "{\"type\":\"rich\",\"title\":\"T\",\"url\":\"https://x.y\",\"color\":16711680,\"description\":\"**d**\","
                "\"provider\":{\"name\":\"P\"},\"author\":{\"name\":\"A\"},\"footer\":{\"text\":\"F\"},"
                "\"fields\":[{\"name\":\"n1\",\"value\":\"v1\",\"inline\":true},{\"name\":\"n2\",\"value\":\"v2\"}],"
                "\"thumbnail\":{\"url\":\"https://t\",\"proxy_url\":\"https://media.discordapp.net/t\",\"width\":80,\"height\":80}},"
                "{\"type\":\"gifv\",\"url\":\"https://tenor\",\"thumbnail\":{\"proxy_url\":\"https://media.discordapp.net/g\","
                "\"width\":498,\"height\":280}},"
                "{\"type\":\"link\",\"url\":\"https://empty\"},"
                "{\"type\":\"video\",\"title\":\"Clip\",\"thumbnail\":{\"proxy_url\":\"https://media.discordapp.net/v\","
                "\"width\":1280,\"height\":720}}]}", &m) && m.nembeds == 3,
          "embeds without content are dropped");
    check(m.nembeds == 3 && m.embeds[0].has_color && m.embeds[0].color == 0xFF0000 && str_eq(&m.embeds[0].title, "T") &&
              str_eq(&m.embeds[0].provider, "P") && str_eq(&m.embeds[0].author, "A") && str_eq(&m.embeds[0].footer, "F") &&
              str_eq(&m.embeds[0].description, "**d**"),
          "rich embed text");
    check(m.nembeds == 3 && m.embeds[0].nfields == 2 && m.embeds[0].fields[0].inline_ && !m.embeds[0].fields[1].inline_ &&
              str_eq(&m.embeds[0].fields[1].value, "v2") && str_eq(&m.embeds[0].thumbnail, "https://media.discordapp.net/t"),
          "embed fields and thumbnail");
    check(m.nembeds == 3 && m.embeds[1].media_only && str_eq(&m.embeds[1].image, "https://media.discordapp.net/g") &&
              m.embeds[1].image_w == 498,
          "GIF embeds show only the media");
    check(m.nembeds == 3 && !m.embeds[2].media_only && str_eq(&m.embeds[2].image, "https://media.discordapp.net/v") &&
              !m.embeds[2].thumbnail.len && m.embeds[2].image_h == 720,
          "video embeds show their thumbnail large");
    msg_free(&m);

    check(parse("{\"id\":\"3\",\"content\":\"\",\"sticker_items\":[{\"id\":\"77\",\"name\":\"wave\",\"format_type\":1}],"
                "\"reactions\":[{\"count\":3,\"me\":true,\"emoji\":{\"id\":null,\"name\":\"\xF0\x9F\x91\x8D\"}},"
                "{\"count\":1,\"me\":false,\"emoji\":{\"id\":\"55\",\"name\":\"pog\"}}]}", &m) &&
              !m.system && lstrcmpA(m.sticker_id, "77") == 0 && m.sticker_format == 1 && str_eq(&m.sticker_name, "wave"),
          "sticker");
    check(m.nreactions == 2 && m.reactions[0].count == 3 && m.reactions[0].me && !m.reactions[0].emoji_id[0] &&
              lstrcmpA(m.reactions[1].emoji_id, "55") == 0 && str_eq(&m.reactions[1].emoji, "pog"),
          "reactions");
    {
        sb_t path = {0};
        msg_reaction_path(&m.reactions[0], &path);
        check(str_eq(&path, "%F0%9F%91%8D"), "unicode reaction is percent-encoded");
        sb_clear(&path);
        msg_reaction_path(&m.reactions[1], &path);
        check(str_eq(&path, "pog:55"), "custom reaction is name:id");
        sb_free(&path);
    }
    msg_free(&m);
}

static void test_reaction_event(void)
{
    static const char ev[] = "{\"user_id\":\"9\",\"channel_id\":\"5\",\"message_id\":\"6\","
                             "\"emoji\":{\"id\":null,\"name\":\"\xE2\x9D\xA4\"}}";
    json_t v;
    msg_batch_t *b;

    json_parse(ev, sizeof ev - 1, &v);
    b = msg_batch_reaction(v, 1, "9");
    check(b->n == 1 && b->kind == BATCH_REACTION && b->delta == 1 && b->mine && lstrcmpA(b->channel_id, "5") == 0 &&
              lstrcmpA(b->msgs[0].id, "6") == 0 && str_eq(&b->msgs[0].reactions[0].emoji, "\xE2\x9D\xA4"),
          "reaction event");
    msg_batch_free(b);
    b = msg_batch_reaction(v, -1, "8");
    check(b->n == 1 && b->delta == -1 && !b->mine, "reaction removed by someone else");
    msg_batch_free(b);
}

static void test_poll(void)
{
    msg_t m;

    check(parse("{\"id\":\"1\",\"content\":\"\",\"poll\":{\"question\":{\"text\":\"Best?\"},\"allow_multiselect\":false,"
                "\"expiry\":\"2026-09-29T12:00:00.000000+00:00\",\"answers\":["
                "{\"answer_id\":1,\"poll_media\":{\"text\":\"Cats\",\"emoji\":{\"id\":null,\"name\":\"\xF0\x9F\x90\xB1\"}}},"
                "{\"answer_id\":2,\"poll_media\":{\"text\":\"Dogs\"}}],"
                "\"results\":{\"is_finalized\":false,\"answer_counts\":[{\"id\":1,\"count\":3,\"me_voted\":true},"
                "{\"id\":2,\"count\":1,\"me_voted\":false}]}}}", &m) &&
              m.poll && !m.system,
          "poll parses");
    check(m.poll && str_eq(&m.poll->question, "Best?") && m.poll->nanswers == 2 && !m.poll->multi && !m.poll->final,
          "poll question and answers");
    check(m.poll && m.poll->nanswers == 2 && str_eq(&m.poll->answers[0].text, "\xF0\x9F\x90\xB1 Cats") &&
              m.poll->answers[0].count == 3 && m.poll->answers[0].me && m.poll->answers[1].count == 1,
          "poll results and our vote");
    check(m.poll && m.poll->expiry_ms == 1790683200000ll, "poll expiry");
    msg_free(&m);
}

static void test_search(void)
{
    static const char res[] = "{\"total_results\":7,\"messages\":["
                              "[{\"id\":\"5\",\"channel_id\":\"9\",\"content\":\"hello there\"}],"
                              "[{\"id\":\"6\",\"content\":\"before\"},{\"id\":\"7\",\"content\":\"the hit\",\"hit\":true}]]}";
    json_t v;
    msg_batch_t *b;

    json_parse(res, sizeof res - 1, &v);
    b = msg_batch_search(v);
    check(b->kind == BATCH_SEARCH && b->total == 7 && b->n == 2, "search results");
    check(b->n == 2 && lstrcmpA(b->msgs[0].id, "5") == 0 && lstrcmpA(b->msgs[0].channel_id, "9") == 0 &&
              lstrcmpA(b->msgs[1].id, "7") == 0 && str_eq(&b->msgs[1].text, "the hit"),
          "the flagged hit is kept from each group");
    msg_batch_free(b);
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
    test_parts();
    test_reaction_event();
    test_poll();
    test_search();
    test_batch();
    finish();
}
