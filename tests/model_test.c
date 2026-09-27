/* Model tests on a hand-written READY: ordering, permissions, DMs, read state and mutes. */
#include <windows.h>
#include "test.h"
#include "model.h"

static const char k_ready[] =
    "{\"user\":{\"id\":\"100\",\"username\":\"me\"},"
    "\"guilds\":[{\"id\":\"1\",\"name\":\"G\",\"owner_id\":\"999\","
    "\"roles\":[{\"id\":\"1\",\"permissions\":\"1024\"},{\"id\":\"50\",\"permissions\":\"0\"}],"
    "\"members\":[{\"user\":{\"id\":\"100\"},\"roles\":[\"50\"]}],"
    "\"channels\":["
    "{\"id\":\"10\",\"type\":4,\"name\":\"Cat\",\"position\":0},"
    "{\"id\":\"11\",\"type\":0,\"name\":\"general\",\"parent_id\":\"10\",\"position\":1,\"last_message_id\":\"500\"},"
    "{\"id\":\"12\",\"type\":0,\"name\":\"secret\",\"parent_id\":\"10\",\"position\":2,"
    "\"permission_overwrites\":[{\"id\":\"1\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]},"
    "{\"id\":\"13\",\"type\":2,\"name\":\"voice\",\"parent_id\":\"10\",\"position\":0},"
    "{\"id\":\"14\",\"type\":0,\"name\":\"top\",\"position\":5,\"last_message_id\":\"300\"},"
    "{\"id\":\"15\",\"type\":0,\"name\":\"staff\",\"position\":6,\"permission_overwrites\":["
    "{\"id\":\"1\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"},{\"id\":\"50\",\"type\":0,\"allow\":\"1024\",\"deny\":\"0\"}]}"
    "]}],"
    "\"private_channels\":["
    "{\"id\":\"20\",\"type\":1,\"last_message_id\":\"100\",\"recipients\":[{\"id\":\"7\",\"username\":\"old\"}]},"
    "{\"id\":\"21\",\"type\":1,\"last_message_id\":\"900\",\"recipients\":[{\"id\":\"8\",\"username\":\"n\",\"global_name\":\"New\",\"avatar\":\"h\"}]},"
    "{\"id\":\"22\",\"type\":3,\"name\":null,\"last_message_id\":\"800\",\"recipients\":[{\"id\":\"7\",\"username\":\"a\"},{\"id\":\"8\",\"username\":\"b\"}]}"
    "],"
    "\"read_state\":{\"entries\":["
    "{\"id\":\"11\",\"last_message_id\":\"400\",\"mention_count\":2},"
    "{\"id\":\"14\",\"last_message_id\":\"300\"},"
    "{\"id\":\"21\",\"last_message_id\":\"850\",\"mention_count\":1}]},"
    "\"user_guild_settings\":{\"entries\":[{\"guild_id\":\"1\",\"muted\":false,"
    "\"channel_overrides\":[{\"channel_id\":\"14\",\"muted\":true}]}]}"
    "}";

static const char *name_at(const model_t *m, unsigned i)
{
    return model_str(m, m->channels[i].name);
}

/* Applies `json` as `event`; returns the new model (NULL if unchanged). */
static model_t *apply(const model_t *m, const char *event, const char *json)
{
    json_t d;

    if (!json_parse(json, sc_strlen(json), &d))
        return NULL;
    return model_apply(m, event, d);
}

static int index_in_guild(const model_t *m, const char *id)
{
    int i = model_find_channel(m, id);
    return i < 0 ? -1 : i - (int)m->guilds[0].first;
}

static void test_updates(const model_t *m)
{
    model_t *n, *n2;
    int i;

    n = apply(m, "CHANNEL_CREATE",
              "{\"id\":\"16\",\"guild_id\":\"1\",\"type\":0,\"name\":\"new\",\"parent_id\":\"10\",\"position\":0}");
    check(n && n->guilds[0].count == 6 && index_in_guild(n, "16") == 3 && index_in_guild(n, "11") == 4,
          "created channel lands in its category, sorted");
    i = n ? model_find_channel(n, "11") : -1;
    check(i >= 0 && n->channels[i].mentions == 2 && model_unread(n, (unsigned)i), "read state survives an update");
    model_free(n);

    n = apply(m, "CHANNEL_CREATE", "{\"id\":\"17\",\"guild_id\":\"1\",\"type\":0,\"name\":\"hidden\","
                                   "\"permission_overwrites\":[{\"id\":\"1\",\"type\":0,\"allow\":\"0\",\"deny\":\"1024\"}]}");
    check(n == NULL, "a channel we cannot see changes nothing");
    model_free(n);

    n = apply(m, "CHANNEL_UPDATE", "{\"id\":\"14\",\"guild_id\":\"1\",\"type\":0,\"name\":\"renamed\",\"position\":5}");
    i = n ? model_find_channel(n, "14") : -1;
    check(i >= 0 && lstrcmpA(model_str(n, n->channels[i].name), "renamed") == 0 && n->channels[i].muted,
          "rename keeps the mute");
    model_free(n);

    n = apply(m, "CHANNEL_DELETE", "{\"id\":\"11\",\"guild_id\":\"1\",\"type\":0}");
    check(n && model_find_channel(n, "11") < 0 && n->guilds[0].count == 4, "deleted channel is gone");
    model_free(n);

    n = apply(m, "CHANNEL_CREATE", "{\"id\":\"23\",\"type\":1,\"last_message_id\":\"950\","
                                   "\"recipients\":[{\"id\":\"9\",\"username\":\"fresh\"}]}");
    check(n && n->dm_count == 4 && lstrcmpA(n->channels[n->dm_first].id, "23") == 0 &&
          lstrcmpA(model_str(n, n->channels[n->dm_first].name), "fresh") == 0, "new DM goes on top");
    model_free(n);

    n = apply(m, "GUILD_CREATE", "{\"id\":\"2\",\"name\":\"Joined\",\"roles\":[{\"id\":\"2\",\"permissions\":\"1024\"}],"
                                 "\"channels\":[{\"id\":\"30\",\"type\":0,\"name\":\"welcome\"}]}");
    check(n && n->nguilds == 2 && lstrcmpA(n->guilds[0].id, "2") == 0 && n->guilds[0].count == 1 &&
          model_find_channel(n, "11") >= 0, "joined server goes on top, others kept");
    n2 = n ? apply(n, "GUILD_DELETE", "{\"id\":\"2\"}") : NULL;
    check(n2 && n2->nguilds == 1 && model_find_channel(n2, "30") < 0, "left server is removed");
    model_free(n2);
    n2 = n ? apply(n, "GUILD_DELETE", "{\"id\":\"2\",\"unavailable\":true}") : NULL;
    check(n2 == NULL, "outage keeps the server");
    model_free(n2);
    model_free(n);

    n = apply(m, "GUILD_MEMBER_UPDATE", "{\"guild_id\":\"1\",\"user\":{\"id\":\"100\"},\"roles\":[\"77\"]}");
    check(n && model_has_role(n, 0, "77") && !model_has_role(n, 0, "50"), "our role change is tracked");
    model_free(n);
    n = apply(m, "GUILD_MEMBER_UPDATE", "{\"guild_id\":\"1\",\"user\":{\"id\":\"5\"},\"roles\":[\"77\"]}");
    check(n == NULL, "someone else's role change is ignored");
    model_free(n);

    n = apply(m, "GUILD_UPDATE", "{\"id\":\"1\",\"name\":\"G2\",\"icon\":\"abc\"}");
    check(n && lstrcmpA(model_str(n, n->guilds[0].name), "G2") == 0 && lstrcmpA(n->guilds[0].icon, "abc") == 0,
          "server rename and icon");
    model_free(n);
}

static void test_threads(const model_t *m)
{
    model_t *a, *b, *c;
    int i;

    a = apply(m, "THREAD_CREATE", "{\"id\":\"90\",\"guild_id\":\"1\",\"parent_id\":\"11\",\"type\":11,\"name\":\"side chat\","
                                  "\"member\":{\"id\":\"90\",\"user_id\":\"100\"},\"thread_metadata\":{\"archived\":false}}");
    check(a != NULL, "joined thread is added");
    i = a ? model_find_channel(a, "90") : -1;
    check(i > 0 && lstrcmpA(a->channels[i - 1].id, "11") == 0 && model_is_thread(a->channels[i].type),
          "thread sits right under its channel");
    check(!apply(m, "THREAD_CREATE", "{\"id\":\"91\",\"guild_id\":\"1\",\"parent_id\":\"11\",\"type\":11,\"name\":\"x\","
                                     "\"owner_id\":\"5\"}"),
          "threads we are not in are left out");
    b = a ? apply(a, "THREAD_UPDATE", "{\"id\":\"90\",\"guild_id\":\"1\",\"parent_id\":\"11\",\"type\":11,\"name\":\"s\","
                                      "\"thread_metadata\":{\"archived\":true}}")
          : NULL;
    check(b && model_find_channel(b, "90") < 0, "archived thread leaves the list");
    c = a ? apply(a, "CHANNEL_UPDATE", "{\"id\":\"11\",\"guild_id\":\"1\",\"type\":0,\"name\":\"general\","
                                       "\"topic\":\"Talk here\",\"parent_id\":\"10\",\"position\":1}")
          : NULL;
    i = c ? model_find_channel(c, "11") : -1;
    check(i >= 0 && lstrcmpA(model_str(c, c->channels[i].topic), "Talk here") == 0 && model_find_channel(c, "90") == i + 1,
          "channel topic, and its thread stays under it");
    model_free(a);
    model_free(b);
    model_free(c);
}

static void test_emojis(const model_t *m)
{
    model_t *a = apply(m, "GUILD_EMOJIS_UPDATE", "{\"guild_id\":\"1\",\"emojis\":["
                                               "{\"id\":\"70\",\"name\":\"pog\",\"animated\":false},"
                                               "{\"id\":\"71\",\"name\":\"dance\",\"animated\":true},"
                                               "{\"id\":\"72\",\"name\":\"gone\",\"available\":false}]}");
    model_emoji_t e;
    unsigned cursor = 0;
    int n = 0, ok = 1;

    check(a != NULL, "emoji update applies");
    while (a && model_emoji_next(a, 0, &cursor, &e)) {
        if (n == 0)
            ok &= lstrcmpA(e.id, "70") == 0 && !e.animated && e.name_len == 3;
        if (n == 1)
            ok &= lstrcmpA(e.id, "71") == 0 && e.animated && e.name_len == 5;
        n++;
    }
    check(n == 2 && ok, "custom emoji listed, unavailable ones dropped");
    model_free(a);
}

static void test_roles(const model_t *m)
{
    model_t *a, *b, *c;
    model_role_t r;
    unsigned cursor = 0;
    int n = 0;

    while (model_role_next(m, 0, &cursor, &r))
        n++;
    check(n == 2, "roles are listed");

    a = apply(m, "GUILD_ROLE_CREATE", "{\"guild_id\":\"1\",\"role\":{\"id\":\"60\",\"name\":\"Mods\",\"color\":16711680,"
                                       "\"position\":5,\"hoist\":true,\"permissions\":\"0\"}}");
    b = a ? apply(a, "GUILD_ROLE_CREATE", "{\"guild_id\":\"1\",\"role\":{\"id\":\"61\",\"name\":\"Admins\","
                                          "\"colors\":{\"primary_color\":255},\"position\":9,\"permissions\":\"0\"}}")
          : NULL;
    check(a && b, "role creates apply");
    if (b) {
        int found = 0;
        cursor = 0;
        while (model_role_next(b, 0, &cursor, &r))
            if (lstrcmpA(r.id, "60") == 0)
                found = r.color == 0xFF0000 && r.position == 5 && r.hoist && r.name_len == 4;
        check(found, "role fields");
        check(model_role_color(b, 0, "50,60") == 0xFF0000, "color of the only colored role");
        check(model_role_color(b, 0, "60,61") == 0x0000FF, "highest colored role wins");
        check(model_role_color(b, 0, "50") == 0 && model_role_color(b, 0, "") == 0, "no color without colored roles");
        c = apply(b, "GUILD_ROLE_DELETE", "{\"guild_id\":\"1\",\"role_id\":\"61\"}");
        check(c && model_role_color(c, 0, "60,61") == 0xFF0000, "deleted role loses its color");
        model_free(c);
        c = apply(b, "GUILD_ROLE_UPDATE", "{\"guild_id\":\"1\",\"role\":{\"id\":\"60\",\"name\":\"Mods\",\"color\":65280,"
                                          "\"position\":20,\"permissions\":\"0\"}}");
        check(c && model_role_color(c, 0, "60,61") == 0x00FF00, "updated role takes its new color and position");
        model_free(c);
    }
    model_free(a);
    model_free(b);
}

void entry(void)
{
    json_t d;
    model_t *m;
    const guild_t *g;
    int i;

    check(json_parse(k_ready, sizeof k_ready - 1, &d), "fixture parses");
    m = model_from_ready(d);
    g = &m->guilds[0];

    check(m->nguilds == 1 && g->count == 5, "hidden channel is dropped");
    check(g->count == 5 && lstrcmpA(name_at(m, g->first), "top") == 0 && lstrcmpA(name_at(m, g->first + 1), "staff") == 0 &&
          lstrcmpA(name_at(m, g->first + 2), "Cat") == 0 && lstrcmpA(name_at(m, g->first + 3), "general") == 0 &&
          lstrcmpA(name_at(m, g->first + 4), "voice") == 0,
          "order: loose channels, then category with text before voice");
    check(model_find_channel(m, "12") < 0, "@everyone deny hides the channel");
    check(model_find_channel(m, "15") >= 0, "role allow overrides @everyone deny");

    check(m->dm_count == 3 && lstrcmpA(name_at(m, m->dm_first), "New") == 0 &&
          lstrcmpA(name_at(m, m->dm_first + 1), "a, b") == 0 && lstrcmpA(name_at(m, m->dm_first + 2), "old") == 0,
          "DMs most recent first, groups named after members");
    check(m->dm_count == 3 && lstrcmpA(m->channels[m->dm_first].user_id, "8") == 0 &&
          lstrcmpA(m->channels[m->dm_first].avatar, "h") == 0, "DM keeps the recipient avatar");

    i = model_find_channel(m, "11");
    check(i >= 0 && model_unread(m, (unsigned)i) && m->channels[i].mentions == 2, "unread channel with mentions");
    i = model_find_channel(m, "14");
    check(i >= 0 && !model_unread(m, (unsigned)i) && m->channels[i].muted, "read and muted channel");
    i = model_find_channel(m, "21");
    check(i >= 0 && model_unread(m, (unsigned)i) && m->channels[i].mentions == 1, "unread DM");
    i = model_find_channel(m, "20");
    check(i >= 0 && !model_unread(m, (unsigned)i), "no read state means read");
    check(model_channel_guild(m, (unsigned)model_find_channel(m, "11")) == 0 &&
          model_channel_guild(m, m->dm_first) == -1, "channel to guild lookup");
    check(model_id_cmp("900", "1000") < 0 && model_id_cmp("1000", "999") > 0, "snowflakes compare as numbers");
    check(model_has_role(m, 0, "50") && !model_has_role(m, 0, "1"), "our roles are remembered");

    test_updates(m);
    test_roles(m);
    test_emojis(m);
    test_threads(m);
    model_free(m);
    finish();
}
