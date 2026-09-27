#include "msg.h"
#include "md.h"
#include "mem.h"
#include "sc_asm.h"

#define REPLY_SNIPPET 100
#define DISCORD_EPOCH 1420070400000ll

enum {
    TYPE_DEFAULT = 0,
    TYPE_PIN = 6,
    TYPE_JOIN = 7,
    TYPE_BOOST = 8,
    TYPE_BOOST_TIER_3 = 11,
    TYPE_REPLY = 19,
    TYPE_SLASH_COMMAND = 20,
    TYPE_CONTEXT_COMMAND = 23,
};

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int same(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* Display name of a user object: global_name, else username. */
static void user_name(json_t user, sb_t *out)
{
    json_t v;

    if ((json_get(user, "global_name", &v) && json_type(v) == JSON_STRING) ||
        json_get(user, "username", &v))
        json_str(v, out);
}

static int mention_name(json_t mentions, const char *id, size_t len, sb_t *out)
{
    json_iter_t it;
    json_t user, v;
    char got[24];

    json_iter(mentions, &it);
    while (json_next(&it, NULL, &user)) {
        if (!json_get(user, "id", &v))
            continue;
        json_raw(v, got, sizeof got);
        if (sc_strlen(got) == len && same(got, id, len)) {
            sb_add(out, MD_MENTION_OPEN "@");
            user_name(user, out);
            sb_add(out, MD_MENTION_CLOSE);
            return 1;
        }
    }
    return 0;
}

/*
 * Rewrites Discord markup into readable text:
 *   <@id> <@!id>  -> @name (from the mentions array), wrapped in MD_MENTION_OPEN/CLOSE
 *   <@&id>        -> @role, wrapped the same way
 *   <:name:id>    -> :name:   (custom emoji, also <a:name:id>)
 * Channel mentions <#id> are left for the UI, which knows channel names.
 */
static void format_content(const char *s, size_t n, json_t mentions, sb_t *out)
{
    size_t i = 0;

    while (i < n) {
        size_t start = i, j;

        if (s[i] != '<') {
            while (i < n && s[i] != '<')
                i++;
            sb_addn(out, s + start, i - start);
            continue;
        }
        /* <@123>, <@!123>, <@&123> */
        if (i + 2 < n && s[i + 1] == '@') {
            int role = s[i + 2] == '&', nick = s[i + 2] == '!';
            j = i + 2 + (role || nick);
            while (j < n && is_digit(s[j]))
                j++;
            if (j < n && s[j] == '>' && j > i + 2 + (role || nick)) {
                size_t id0 = i + 2 + (role || nick);
                if (role)
                    sb_add(out, MD_MENTION_OPEN "@role" MD_MENTION_CLOSE);
                else if (!mention_name(mentions, s + id0, j - id0, out))
                    sb_add(out, MD_MENTION_OPEN "@unknown-user" MD_MENTION_CLOSE);
                i = j + 1;
                continue;
            }
        }
        /* <:name:123> and <a:name:123> */
        j = i + 1 + (i + 1 < n && s[i + 1] == 'a');
        if (j < n && s[j] == ':') {
            size_t name0 = j + 1, k = name0;
            while (k < n && s[k] != ':' && s[k] != '>' && s[k] != ' ')
                k++;
            if (k < n && s[k] == ':' && k > name0) {
                size_t d = k + 1;
                while (d < n && is_digit(s[d]))
                    d++;
                if (d < n && s[d] == '>' && d > k + 1) {
                    sb_addn(out, s + name0 - 1, k - name0 + 2); /* ":name:" */
                    i = d + 1;
                    continue;
                }
            }
        }
        sb_addn(out, "<", 1);
        i++;
    }
}

/* Appends at most `max` bytes of the first line of `s`, cut on a UTF-8 boundary. */
static void first_line(const char *s, size_t n, size_t max, sb_t *out)
{
    size_t len = 0;

    while (len < n && s[len] != '\n')
        len++;
    if (len > max) {
        len = max;
        while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80)
            len--;
        sb_addn(out, s, len);
        sb_add(out, "\xE2\x80\xA6");
    } else {
        sb_addn(out, s, len);
    }
}

static void add_line(sb_t *text, const char *prefix, json_t name)
{
    if (text->len)
        sb_add(text, "\n");
    sb_add(text, prefix);
    json_str(name, text);
}

static void parse_reply(json_t obj, sb_t *out)
{
    json_t ref, author, content;
    sb_t raw = {0};

    if (!json_get(obj, "referenced_message", &ref) || json_type(ref) != JSON_OBJECT)
        return;
    if (json_get(ref, "author", &author))
        user_name(author, out);
    sb_add(out, ": ");
    if (json_get(ref, "content", &content) && json_str(content, &raw) && raw.len)
        first_line(raw.data, raw.len, REPLY_SNIPPET, out);
    else
        sb_add(out, "(attachment)");
    sb_free(&raw);
}

int msg_parse(json_t obj, msg_t *out)
{
    json_t v, author, mentions = {0}, list, item, name;
    json_iter_t it;
    sb_t raw = {0};
    long long type = 0;

    if (json_type(obj) != JSON_OBJECT || !json_get(obj, "id", &v))
        return 0;
    json_raw(v, out->id, sizeof out->id);
    if (json_get(obj, "channel_id", &v))
        json_raw(v, out->channel_id, sizeof out->channel_id);
    if (json_get(obj, "type", &v))
        json_int(v, &type);

    if (json_get(obj, "author", &author)) {
        json_t member, nick;
        if (json_get(author, "id", &v))
            json_raw(v, out->author_id, sizeof out->author_id);
        if (json_get(author, "avatar", &v))
            json_raw(v, out->avatar, sizeof out->avatar);
        /* Gateway messages carry the server nickname. */
        if (json_get(obj, "member", &member) && json_get(member, "nick", &nick) && json_type(nick) == JSON_STRING)
            json_str(nick, &out->author);
        else
            user_name(author, &out->author);
    }

    json_get(obj, "mentions", &mentions);
    if (json_get(obj, "content", &v) && json_str(v, &raw))
        format_content(raw.data ? raw.data : "", raw.len, mentions, &out->text);
    sb_free(&raw);

    if (json_get(obj, "attachments", &list)) {
        json_iter(list, &it);
        while (json_next(&it, NULL, &item))
            if (json_get(item, "filename", &name))
                add_line(&out->text, "\xF0\x9F\x93\x8E ", name);
    }
    if (json_get(obj, "sticker_items", &list)) {
        json_iter(list, &it);
        while (json_next(&it, NULL, &item))
            if (json_get(item, "name", &name))
                add_line(&out->text, "Sticker: ", name);
    }
    if (!out->text.len && json_get(obj, "embeds", &list)) {
        json_iter(list, &it);
        if (json_next(&it, NULL, &item) &&
            (json_get(item, "title", &name) || json_get(item, "description", &name)))
            add_line(&out->text, "Embed: ", name);
    }

    if (type == TYPE_JOIN) {
        out->system = 1;
        sb_clear(&out->text);
        sb_add(&out->text, "joined the server.");
    } else if (type >= TYPE_BOOST && type <= TYPE_BOOST_TIER_3) {
        out->system = 1;
        sb_clear(&out->text);
        sb_add(&out->text, "boosted the server.");
    } else if (type == TYPE_PIN) {
        out->system = 1;
        sb_clear(&out->text);
        sb_add(&out->text, "pinned a message.");
    } else if (type != TYPE_DEFAULT && type != TYPE_REPLY && type != TYPE_SLASH_COMMAND &&
               type != TYPE_CONTEXT_COMMAND && !out->text.len) {
        out->system = 1;
        sb_add(&out->text, "sent a system message.");
    }
    parse_reply(obj, &out->reply);
    return 1;
}

void msg_free(msg_t *m)
{
    sb_free(&m->author);
    sb_free(&m->text);
    sb_free(&m->reply);
}

msg_batch_t *msg_batch_from_array(json_t arr, int kind, const char *channel_id, int limit)
{
    msg_batch_t *b = mem_alloc(sizeof *b);
    json_iter_t it;
    json_t obj;
    int total = (int)json_count(arr), i = 0;

    b->kind = kind;
    for (int k = 0; k < (int)sizeof b->channel_id - 1 && channel_id[k]; k++)
        b->channel_id[k] = channel_id[k];
    b->msgs = mem_alloc(((size_t)total + 1) * sizeof *b->msgs);
    json_iter(arr, &it);
    while (i < total && json_next(&it, NULL, &obj)) {
        msg_t *m = &b->msgs[total - 1 - i];
        if (msg_parse(obj, m)) {
            i++;
        } else {
            msg_free(m);
            *m = (msg_t){0};
        }
    }
    /* Items that failed to parse leave holes at the front: compact. */
    if (i < total) {
        for (int k = 0; k < i; k++)
            b->msgs[k] = b->msgs[total - i + k];
    }
    b->n = i;
    b->has_more = total >= limit;
    return b;
}

msg_batch_t *msg_batch_one(json_t obj, int kind)
{
    msg_batch_t *b = mem_alloc(sizeof *b);

    b->kind = kind;
    b->msgs = mem_alloc(sizeof *b->msgs);
    if (msg_parse(obj, &b->msgs[0])) {
        b->n = 1;
        for (int k = 0; k < (int)sizeof b->channel_id; k++)
            b->channel_id[k] = b->msgs[0].channel_id[k];
    } else {
        msg_free(&b->msgs[0]);
    }
    return b;
}

void msg_batch_free(msg_batch_t *b)
{
    if (!b)
        return;
    for (int i = 0; i < b->n; i++)
        msg_free(&b->msgs[i]);
    mem_free(b->msgs);
    mem_free(b);
}

long long snowflake_ms(const char *id)
{
    unsigned long long n = 0;

    for (; is_digit(*id); id++)
        n = n * 10 + (unsigned long long)(*id - '0');
    return (long long)(n >> 22) + DISCORD_EPOCH;
}
