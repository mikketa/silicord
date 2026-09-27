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

    model_free(m);
    finish();
}
