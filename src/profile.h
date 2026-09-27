#pragma once
#include "json.h"
#include "sb.h"

/*
 * What a profile popout shows, from GET /users/{id}/profile. When the profile
 * was asked for in a server, the server profile (nickname, avatar, banner,
 * bio, pronouns) replaces the global one where it is set, as Discord does.
 */

#define PROFILE_MAX_COLORS 5
#define PROFILE_MAX_FRIENDS 3

/* display_name_styles.effect_id */
enum {
    NAME_SOLID = 1,
    NAME_GRADIENT = 2,
    NAME_NEON = 3,
    NAME_TOON = 4,
    NAME_POP = 5,
    NAME_GLOW = 6,
    NAME_PRISM = 7,
    NAME_GUMMY = 8,
};

typedef struct {
    char id[48];
    char icon[48];        /* /badge-icons/{icon}.png */
    sb_t description;
    sb_t link;
} profile_badge_t;

typedef struct {
    char id[24];
    char avatar[48];
} profile_friend_t;

typedef struct {
    char id[24];
    char guild_id[24];    /* server the profile was asked for, empty for the global one */
    sb_t username;
    sb_t name;            /* nickname, else global name, else username */
    sb_t bio;
    sb_t pronouns;

    /* Images. The member_* ones live under /guilds/{guild_id}/users/{id}/. */
    char avatar[48];
    char banner[48];
    int member_avatar, member_banner;
    char decoration[64];  /* /avatar-decoration-presets/{decoration}.png */

    /* Colors, 0xRRGGBB. */
    int has_accent;
    unsigned accent;
    int ntheme;
    unsigned theme[2];    /* primary (banner and body), accent (edges) */

    /* Display name style. */
    int font_id, effect_id, ncolors;
    unsigned colors[PROFILE_MAX_COLORS];

    /* Server tag shown next to the username. */
    char tag[24];
    char tag_badge[48];   /* /guild-tag-badges/{tag_guild}/{tag_badge}.png */
    char tag_guild[24];

    profile_badge_t *badges;
    int nbadges;

    int mutual_guilds;    /* -1 when unknown */
    int mutual_friends;
    profile_friend_t friends[PROFILE_MAX_FRIENDS];
    int nfriends;
} profile_t;

/* Parses a profile response; `guild_id` is the server it was asked for, or NULL. */
int profile_parse(json_t root, const char *guild_id, profile_t *out);
void profile_free(profile_t *p);
