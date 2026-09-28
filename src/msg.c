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
        int animated = i + 1 < n && s[i + 1] == 'a';
        j = i + 1 + animated;
        if (j < n && s[j] == ':') {
            size_t name0 = j + 1, k = name0;
            while (k < n && s[k] != ':' && s[k] != '>' && s[k] != ' ')
                k++;
            if (k < n && s[k] == ':' && k > name0) {
                size_t d = k + 1;
                while (d < n && is_digit(s[d]))
                    d++;
                if (d < n && s[d] == '>' && d > k + 1) {
                    sb_add(out, MD_EMOJI_OPEN);
                    if (animated)
                        sb_add(out, "a");
                    sb_addn(out, s + k + 1, d - k - 1);
                    sb_add(out, ":");
                    sb_addn(out, s + name0, k - name0);
                    sb_add(out, MD_EMOJI_CLOSE);
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

static int get_sb(json_t obj, const char *key, sb_t *out)
{
    json_t v;

    return json_get(obj, key, &v) && json_type(v) == JSON_STRING && json_str(v, out);
}

static int get_num(json_t obj, const char *key)
{
    json_t v;
    long long n = 0;

    return json_get(obj, key, &v) && json_int(v, &n) ? (int)n : 0;
}

static int starts_with(const sb_t *s, const char *prefix)
{
    size_t n = sc_strlen(prefix);

    return s->len >= n && same(s->data, prefix, n);
}

static void parse_files(json_t list, msg_t *out)
{
    json_iter_t it;
    json_t item, v;
    size_t n = json_count(list);

    if (!n)
        return;
    out->files = mem_alloc(n * sizeof *out->files);
    json_iter(list, &it);
    while (json_next(&it, NULL, &item)) {
        msg_file_t *f = &out->files[out->nfiles];
        sb_t type = {0};
        long long size = 0;

        if (!get_sb(item, "filename", &f->name))
            continue;
        get_sb(item, "content_type", &type);
        f->width = get_num(item, "width");
        f->height = get_num(item, "height");
        if (json_get(item, "size", &v))
            json_int(v, &size);
        f->size = size;
        f->spoiler = starts_with(&f->name, "SPOILER_");
        f->image = starts_with(&type, "image/") && f->width > 0 && f->height > 0;
        /* Images go through the media proxy, which can resize them. */
        if (!(f->image && get_sb(item, "proxy_url", &f->url)))
            get_sb(item, "url", &f->url);
        sb_free(&type);
        out->nfiles++;
    }
}

static void parse_media(json_t embed, const char *key, sb_t *url, int *w, int *h)
{
    json_t m;

    if (!json_get(embed, key, &m) || json_type(m) != JSON_OBJECT)
        return;
    if (!get_sb(m, "proxy_url", url))
        get_sb(m, "url", url);
    *w = get_num(m, "width");
    *h = get_num(m, "height");
}

static void parse_embeds(json_t list, msg_t *out)
{
    json_iter_t it, fit;
    json_t item, v, field;
    size_t n = json_count(list);

    if (!n)
        return;
    out->embeds = mem_alloc(n * sizeof *out->embeds);
    json_iter(list, &it);
    while (json_next(&it, NULL, &item)) {
        msg_embed_t *e = &out->embeds[out->nembeds];
        sb_t type = {0};
        long long color;

        get_sb(item, "type", &type);
        if (json_get(item, "color", &v) && json_int(v, &color)) {
            e->has_color = 1;
            e->color = (unsigned)color & 0xFFFFFF;
        }
        if (json_get(item, "provider", &v))
            get_sb(v, "name", &e->provider);
        if (json_get(item, "author", &v))
            get_sb(v, "name", &e->author);
        if (json_get(item, "footer", &v))
            get_sb(v, "text", &e->footer);
        get_sb(item, "title", &e->title);
        get_sb(item, "url", &e->url);
        get_sb(item, "description", &e->description);
        parse_media(item, "image", &e->image, &e->image_w, &e->image_h);
        parse_media(item, "thumbnail", &e->thumbnail, &e->thumb_w, &e->thumb_h);
        /* Pictures and GIFs linked on their own: Discord shows just the media. */
        if (starts_with(&type, "image") || starts_with(&type, "gifv")) {
            e->media_only = 1;
        } else if (starts_with(&type, "video") && !e->image.len && e->thumbnail.len) {
            /* Videos (YouTube...) show their thumbnail large. */
            sb_t t = e->image;
            e->image = e->thumbnail;
            e->thumbnail = t;
            e->image_w = e->thumb_w;
            e->image_h = e->thumb_h;
            e->thumb_w = e->thumb_h = 0;
        }
        if (e->media_only && !e->image.len) {
            sb_t t = e->image;
            e->image = e->thumbnail;
            e->thumbnail = t;
            e->image_w = e->thumb_w;
            e->image_h = e->thumb_h;
        }
        if (json_get(item, "fields", &v) && json_count(v)) {
            e->fields = mem_alloc(json_count(v) * sizeof *e->fields);
            json_iter(v, &fit);
            while (json_next(&fit, NULL, &field)) {
                msg_field_t *f = &e->fields[e->nfields];
                json_t in;
                get_sb(field, "name", &f->name);
                get_sb(field, "value", &f->value);
                f->inline_ = json_get(field, "inline", &in) && json_type(in) == JSON_TRUE;
                e->nfields++;
            }
        }
        sb_free(&type);
        if (e->media_only ? e->image.len > 0
                          : e->title.len || e->description.len || e->author.len || e->image.len || e->nfields)
            out->nembeds++;
        else
            msg_embed_free(e);
    }
}

/* Days from 1970-01-01 of a civil date (Howard Hinnant's algorithm). */
static long long days_from_civil(long long y, int m, int d)
{
    long long era;
    int yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (int)(y - era * 400);
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* "2026-09-28T12:34:56.789+00:00" to Unix ms (UTC offsets other than +00:00 are ignored). */
static long long iso_ms(const char *s)
{
    int f[6] = {0};
    int k = 0;

    for (const char *p = s; *p && k < 6; p++) {
        if (*p >= '0' && *p <= '9') {
            f[k] = f[k] * 10 + (*p - '0');
        } else if (*p == '.' || *p == '+' || *p == 'Z') {
            break;
        } else {
            k++;
        }
    }
    return ((days_from_civil(f[0], f[1], f[2]) * 24 + f[3]) * 60 + f[4]) * 60000ll + f[5] * 1000ll;
}

static void parse_poll(json_t poll, msg_t *out)
{
    msg_poll_t *p = mem_alloc(sizeof *p);
    json_t v, answers, a, media, emoji, results, counts, c;
    json_iter_t it;

    if (json_get(poll, "question", &v))
        get_sb(v, "text", &p->question);
    p->multi = json_get(poll, "allow_multiselect", &v) && json_type(v) == JSON_TRUE;
    if (json_get(poll, "expiry", &v) && json_type(v) == JSON_STRING) {
        sb_t iso = {0};
        json_str(v, &iso);
        p->expiry_ms = iso_ms(iso.data);
        sb_free(&iso);
    }
    if (json_get(poll, "answers", &answers)) {
        p->answers = mem_alloc((json_count(answers) + 1) * sizeof *p->answers);
        json_iter(answers, &it);
        while (json_next(&it, NULL, &a)) {
            msg_answer_t *ans = &p->answers[p->nanswers];
            ans->id = get_num(a, "answer_id");
            if (json_get(a, "poll_media", &media)) {
                if (json_get(media, "emoji", &emoji) && json_get(emoji, "name", &v) && json_type(v) == JSON_STRING &&
                    !(json_get(emoji, "id", &c) && json_type(c) == JSON_STRING)) {
                    json_str(v, &ans->text);
                    sb_add(&ans->text, " ");
                }
                get_sb(media, "text", &ans->text);
            }
            p->nanswers++;
        }
    }
    if (json_get(poll, "results", &results)) {
        p->final = json_get(results, "is_finalized", &v) && json_type(v) == JSON_TRUE;
        if (json_get(results, "answer_counts", &counts)) {
            json_iter(counts, &it);
            while (json_next(&it, NULL, &c)) {
                int id = get_num(c, "id");
                for (int k = 0; k < p->nanswers; k++)
                    if (p->answers[k].id == id) {
                        p->answers[k].count = get_num(c, "count");
                        p->answers[k].me = json_get(c, "me_voted", &v) && json_type(v) == JSON_TRUE;
                    }
            }
        }
    }
    out->poll = p;
}

void msg_poll_free(msg_poll_t *p)
{
    if (!p)
        return;
    sb_free(&p->question);
    for (int i = 0; i < p->nanswers; i++)
        sb_free(&p->answers[i].text);
    mem_free(p->answers);
    mem_free(p);
}

static void parse_reactions(json_t list, msg_t *out)
{
    json_iter_t it;
    json_t item, emoji, v;
    size_t n = json_count(list);

    if (!n)
        return;
    out->reactions = mem_alloc(n * sizeof *out->reactions);
    json_iter(list, &it);
    while (json_next(&it, NULL, &item)) {
        msg_reaction_t *r = &out->reactions[out->nreactions];
        if (!json_get(item, "emoji", &emoji))
            continue;
        if (json_get(emoji, "id", &v) && json_type(v) == JSON_STRING)
            json_raw(v, r->emoji_id, sizeof r->emoji_id);
        get_sb(emoji, "name", &r->emoji);
        r->count = get_num(item, "count");
        r->me = json_get(item, "me", &v) && json_type(v) == JSON_TRUE;
        if (r->count > 0)
            out->nreactions++;
        else
            sb_free(&r->emoji);
    }
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
        /* Gateway messages carry the server nickname and roles. */
        if (json_get(obj, "member", &member) && json_get(member, "nick", &nick) && json_type(nick) == JSON_STRING)
            json_str(nick, &out->author);
        else
            user_name(author, &out->author);
        if (json_get(obj, "member", &member) && json_get(member, "roles", &v)) {
            json_iter_t rit;
            json_t role;
            char id[24];
            out->has_member = 1;
            json_iter(v, &rit);
            while (json_next(&rit, NULL, &role)) {
                json_raw(role, id, sizeof id);
                if (out->member_roles.len)
                    sb_add(&out->member_roles, ",");
                sb_add(&out->member_roles, id);
            }
        }
    }

    json_get(obj, "mentions", &mentions);
    if (json_get(obj, "content", &v) && json_str(v, &raw)) {
        format_content(raw.data ? raw.data : "", raw.len, mentions, &out->text);
        sb_addn(&out->content, raw.data ? raw.data : "", raw.len);
    }
    sb_free(&raw);

    if (json_get(obj, "attachments", &list))
        parse_files(list, out);
    if (json_get(obj, "embeds", &list))
        parse_embeds(list, out);
    if (json_get(obj, "reactions", &list))
        parse_reactions(list, out);
    if (json_get(obj, "poll", &list) && json_type(list) == JSON_OBJECT)
        parse_poll(list, out);
    if (json_get(obj, "sticker_items", &list)) {
        json_iter(list, &it);
        if (json_next(&it, NULL, &item) && json_get(item, "id", &v)) {
            json_raw(v, out->sticker_id, sizeof out->sticker_id);
            out->sticker_format = get_num(item, "format_type");
            if (json_get(item, "name", &name))
                json_str(name, &out->sticker_name);
        }
    }
    out->edited = json_get(obj, "edited_timestamp", &v) && json_type(v) == JSON_STRING;
    out->mention_everyone = json_get(obj, "mention_everyone", &v) && json_type(v) == JSON_TRUE;
    (void)add_line;

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
               type != TYPE_CONTEXT_COMMAND && !out->text.len && !out->nfiles && !out->nembeds &&
               !out->sticker_id[0] && !out->poll) {
        out->system = 1;
        sb_add(&out->text, "sent a system message.");
    }
    parse_reply(obj, &out->reply);
    if (json_get(obj, "message_reference", &v) && json_get(v, "message_id", &name))
        json_raw(name, out->reply_id, sizeof out->reply_id);
    return 1;
}

void msg_embed_free(msg_embed_t *e)
{
    sb_free(&e->provider);
    sb_free(&e->author);
    sb_free(&e->title);
    sb_free(&e->url);
    sb_free(&e->description);
    sb_free(&e->footer);
    sb_free(&e->image);
    sb_free(&e->thumbnail);
    for (int i = 0; i < e->nfields; i++) {
        sb_free(&e->fields[i].name);
        sb_free(&e->fields[i].value);
    }
    mem_free(e->fields);
    *e = (msg_embed_t){0};
}

/* Drops everything but the text fields' storage owned elsewhere: files, embeds, reactions, sticker. */
void msg_free_extras(msg_t *m)
{
    for (int i = 0; i < m->nfiles; i++) {
        sb_free(&m->files[i].url);
        sb_free(&m->files[i].name);
    }
    mem_free(m->files);
    for (int i = 0; i < m->nembeds; i++)
        msg_embed_free(&m->embeds[i]);
    mem_free(m->embeds);
    for (int i = 0; i < m->nreactions; i++)
        sb_free(&m->reactions[i].emoji);
    mem_free(m->reactions);
    sb_free(&m->sticker_name);
    msg_poll_free(m->poll);
    m->poll = NULL;
    m->files = NULL;
    m->embeds = NULL;
    m->reactions = NULL;
    m->nfiles = m->nembeds = m->nreactions = 0;
    m->sticker_id[0] = 0;
}

void msg_free(msg_t *m)
{
    sb_free(&m->author);
    sb_free(&m->text);
    sb_free(&m->reply);
    sb_free(&m->content);
    sb_free(&m->member_roles);
    msg_free_extras(m);
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

msg_batch_t *msg_batch_reaction(json_t d, int delta, const char *me)
{
    msg_batch_t *b = mem_alloc(sizeof *b);
    msg_t *m;
    msg_reaction_t *r;
    json_t v, emoji;
    char user[24] = "";

    b->kind = BATCH_REACTION;
    b->delta = delta;
    b->msgs = mem_alloc(sizeof *b->msgs);
    m = &b->msgs[0];
    if (!json_get(d, "message_id", &v) || !json_get(d, "emoji", &emoji))
        return b;
    json_raw(v, m->id, sizeof m->id);
    if (json_get(d, "channel_id", &v))
        json_raw(v, b->channel_id, sizeof b->channel_id);
    if (json_get(d, "user_id", &v))
        json_raw(v, user, sizeof user);
    b->mine = me && me[0] && sc_strlen(user) == sc_strlen(me) && same(user, me, sc_strlen(me));
    m->reactions = mem_alloc(sizeof *m->reactions);
    r = &m->reactions[0];
    if (json_get(emoji, "id", &v) && json_type(v) == JSON_STRING)
        json_raw(v, r->emoji_id, sizeof r->emoji_id);
    get_sb(emoji, "name", &r->emoji);
    m->nreactions = 1;
    b->n = 1;
    return b;
}

msg_batch_t *msg_batch_poll_vote(json_t d, int delta, const char *me)
{
    msg_batch_t *b = mem_alloc(sizeof *b);
    json_t v;
    char user[24] = "";

    b->kind = BATCH_POLL_VOTE;
    b->delta = delta;
    b->msgs = mem_alloc(sizeof *b->msgs);
    if (!json_get(d, "message_id", &v))
        return b;
    json_raw(v, b->msgs[0].id, sizeof b->msgs[0].id);
    if (json_get(d, "channel_id", &v))
        json_raw(v, b->channel_id, sizeof b->channel_id);
    if (json_get(d, "user_id", &v))
        json_raw(v, user, sizeof user);
    b->mine = me && me[0] && sc_strlen(user) == sc_strlen(me) && same(user, me, sc_strlen(me));
    b->total = get_num(d, "answer_id");
    b->n = 1;
    return b;
}

msg_batch_t *msg_batch_search(json_t root)
{
    msg_batch_t *b = mem_alloc(sizeof *b);
    json_t v, groups, group, hit;
    json_iter_t it, git;
    long long total = 0;

    b->kind = BATCH_SEARCH;
    if (json_get(root, "total_results", &v))
        json_int(v, &total);
    b->total = (int)total;
    if (!json_get(root, "messages", &groups))
        return b;
    b->msgs = mem_alloc((json_count(groups) + 1) * sizeof *b->msgs);
    json_iter(groups, &it);
    while (json_next(&it, NULL, &group)) {
        /* Each group is the hit, or the hit with messages around it: the hit is flagged. */
        json_t pick = {0}, flag;
        json_iter(group, &git);
        while (json_next(&git, NULL, &hit)) {
            if (!pick.p)
                pick = hit;
            if (json_get(hit, "hit", &flag) && json_type(flag) == JSON_TRUE) {
                pick = hit;
                break;
            }
        }
        if (pick.p && msg_parse(pick, &b->msgs[b->n]))
            b->n++;
        else if (pick.p)
            msg_free(&b->msgs[b->n]), b->msgs[b->n] = (msg_t){0};
    }
    return b;
}

static void pct(sb_t *out, const char *s, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            sb_addn(out, (const char *)&c, 1);
        } else {
            char e[3] = {'%', hex[c >> 4], hex[c & 15]};
            sb_addn(out, e, 3);
        }
    }
}

void msg_reaction_path(const msg_reaction_t *r, sb_t *out)
{
    pct(out, r->emoji.data ? r->emoji.data : "", r->emoji.len);
    if (r->emoji_id[0]) {
        sb_add(out, ":");
        sb_add(out, r->emoji_id);
    }
}

long long snowflake_ms(const char *id)
{
    unsigned long long n = 0;

    for (; is_digit(*id); id++)
        n = n * 10 + (unsigned long long)(*id - '0');
    return (long long)(n >> 22) + DISCORD_EPOCH;
}
