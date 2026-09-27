#include <windows.h>
#include "profile.h"
#include "mem.h"

/* Copies a string field into dst; leaves dst alone when it is missing, null or empty. */
static int get_raw(json_t obj, const char *key, char *dst, size_t size)
{
    json_t v;

    if (!json_get(obj, key, &v) || json_type(v) != JSON_STRING || v.end - v.p <= 2)
        return 0;
    json_raw(v, dst, size);
    return 1;
}

/* Replaces `out` with a decoded string field when it is set and not empty. */
static int get_str(json_t obj, const char *key, sb_t *out)
{
    json_t v;

    if (!json_get(obj, key, &v) || json_type(v) != JSON_STRING || v.end - v.p <= 2)
        return 0;
    sb_clear(out);
    json_str(v, out);
    return 1;
}

static int get_color(json_t obj, const char *key, unsigned *out)
{
    json_t v;
    long long n;

    if (!json_get(obj, key, &v) || !json_int(v, &n))
        return 0;
    *out = (unsigned)n & 0xFFFFFF;
    return 1;
}

static int get_colors(json_t obj, const char *key, unsigned *out, int max)
{
    json_t arr, v;
    json_iter_t it;
    long long n;
    int count = 0;

    if (!json_get(obj, key, &arr) || json_type(arr) != JSON_ARRAY)
        return 0;
    json_iter(arr, &it);
    while (count < max && json_next(&it, NULL, &v))
        if (json_int(v, &n))
            out[count++] = (unsigned)n & 0xFFFFFF;
    return count;
}

static int get_int(json_t obj, const char *key, int fallback)
{
    json_t v;
    long long n;

    return json_get(obj, key, &v) && json_int(v, &n) ? (int)n : fallback;
}

static void parse_user(json_t user, profile_t *p)
{
    json_t v;

    get_raw(user, "id", p->id, sizeof p->id);
    get_str(user, "username", &p->username);
    if (!get_str(user, "global_name", &p->name))
        get_str(user, "username", &p->name);
    get_raw(user, "avatar", p->avatar, sizeof p->avatar);
    get_raw(user, "banner", p->banner, sizeof p->banner);
    get_str(user, "bio", &p->bio);
    p->has_accent = get_color(user, "accent_color", &p->accent);

    if (json_get(user, "avatar_decoration_data", &v) && json_type(v) == JSON_OBJECT)
        get_raw(v, "asset", p->decoration, sizeof p->decoration);
    if (json_get(user, "display_name_styles", &v) && json_type(v) == JSON_OBJECT) {
        p->font_id = get_int(v, "font_id", 0);
        p->effect_id = get_int(v, "effect_id", 0);
        p->ncolors = get_colors(v, "colors", p->colors, PROFILE_MAX_COLORS);
    }
    if (json_get(user, "primary_guild", &v) && json_type(v) == JSON_OBJECT) {
        json_t on;
        if (!json_get(v, "identity_enabled", &on) || json_type(on) != JSON_FALSE) {
            get_raw(v, "tag", p->tag, sizeof p->tag);
            get_raw(v, "badge", p->tag_badge, sizeof p->tag_badge);
            get_raw(v, "identity_guild_id", p->tag_guild, sizeof p->tag_guild);
        }
    }
}

/* user_profile and guild_member_profile share their fields; set ones win. */
static void parse_meta(json_t meta, profile_t *p, int member)
{
    unsigned theme[2];
    int n;

    get_str(meta, "bio", &p->bio);
    get_str(meta, "pronouns", &p->pronouns);
    if (get_raw(meta, "banner", p->banner, sizeof p->banner))
        p->member_banner = member;
    if (get_color(meta, "accent_color", &p->accent))
        p->has_accent = 1;
    if ((n = get_colors(meta, "theme_colors", theme, 2)) == 2) {
        p->theme[0] = theme[0];
        p->theme[1] = theme[1];
        p->ntheme = 2;
    }
}

static void parse_badges(json_t arr, profile_t *p)
{
    json_iter_t it;
    json_t b;

    p->nbadges = 0;
    p->badges = mem_alloc((json_count(arr) + 1) * sizeof *p->badges);
    json_iter(arr, &it);
    while (json_next(&it, NULL, &b)) {
        profile_badge_t *d = &p->badges[p->nbadges];
        if (!get_raw(b, "icon", d->icon, sizeof d->icon))
            continue;
        get_raw(b, "id", d->id, sizeof d->id);
        get_str(b, "description", &d->description);
        get_str(b, "link", &d->link);
        p->nbadges++;
    }
}

int profile_parse(json_t root, const char *guild_id, profile_t *out)
{
    json_t v, w;
    json_iter_t it;

    *out = (profile_t){.mutual_guilds = -1, .mutual_friends = -1};
    if (json_type(root) != JSON_OBJECT || !json_get(root, "user", &v) || json_type(v) != JSON_OBJECT)
        return 0;
    parse_user(v, out);
    if (!out->id[0])
        return 0;
    if (json_get(root, "user_profile", &v) && json_type(v) == JSON_OBJECT)
        parse_meta(v, out, 0);

    if (guild_id && guild_id[0]) {
        lstrcpynA(out->guild_id, guild_id, sizeof out->guild_id);
        if (json_get(root, "guild_member", &v) && json_type(v) == JSON_OBJECT) {
            get_str(v, "nick", &out->name);
            if (get_raw(v, "avatar", out->avatar, sizeof out->avatar))
                out->member_avatar = 1;
            if (get_raw(v, "banner", out->banner, sizeof out->banner))
                out->member_banner = 1;
        }
        if (json_get(root, "guild_member_profile", &v) && json_type(v) == JSON_OBJECT)
            parse_meta(v, out, 1);
    }

    if (json_get(root, "badges", &v) && json_type(v) == JSON_ARRAY)
        parse_badges(v, out);
    if (json_get(root, "mutual_guilds", &v) && json_type(v) == JSON_ARRAY)
        out->mutual_guilds = (int)json_count(v);
    if (json_get(root, "mutual_friends", &v) && json_type(v) == JSON_ARRAY) {
        out->mutual_friends = (int)json_count(v);
        json_iter(v, &it);
        while (out->nfriends < PROFILE_MAX_FRIENDS && json_next(&it, NULL, &w)) {
            profile_friend_t *f = &out->friends[out->nfriends];
            if (get_raw(w, "id", f->id, sizeof f->id)) {
                get_raw(w, "avatar", f->avatar, sizeof f->avatar);
                out->nfriends++;
            }
        }
    }
    out->mutual_friends = get_int(root, "mutual_friends_count", out->mutual_friends);
    return 1;
}

void profile_free(profile_t *p)
{
    sb_free(&p->username);
    sb_free(&p->name);
    sb_free(&p->bio);
    sb_free(&p->pronouns);
    for (int i = 0; i < p->nbadges; i++) {
        sb_free(&p->badges[i].description);
        sb_free(&p->badges[i].link);
    }
    mem_free(p->badges);
    *p = (profile_t){0};
}
