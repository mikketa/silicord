#pragma once
#include "json.h"
#include "sb.h"

/*
 * Compact copy of what the UI needs from READY. Strings are offsets into
 * `strings`. The model is immutable: gateway events produce an updated copy
 * with model_apply().
 */

enum {
    CH_TEXT = 0,
    CH_DM = 1,
    CH_VOICE = 2,
    CH_GROUP_DM = 3,
    CH_CATEGORY = 4,
    CH_NEWS = 5,
    CH_NEWS_THREAD = 10,
    CH_PUBLIC_THREAD = 11,
    CH_PRIVATE_THREAD = 12,
    CH_STAGE = 13,
    CH_FORUM = 15,
    CH_MEDIA = 16,
};

/* Which messages notify. */
enum { NOTIFY_DEFAULT, NOTIFY_ALL, NOTIFY_MENTIONS, NOTIFY_NOTHING };

typedef struct {
    char id[24];
    char icon[40];      /* CDN hash, empty if none */
    unsigned name;
    unsigned first;     /* the guild's channels, in display order */
    unsigned count;
    int muted;
    long long mute_until; /* Unix ms when a timed mute ends, 0 if it does not */
    int notify;         /* NOTIFY_*: ours, or NOTIFY_DEFAULT for the server's */
    int default_notify; /* the server's default: NOTIFY_ALL or NOTIFY_MENTIONS */
    int suppress_everyone;
    int suppress_roles;
    /* What we need to decide which new channels are visible. */
    unsigned long long base_perms;
    int sees_all;       /* owner, administrator, or our roles are unknown */
    unsigned my_roles;  /* comma-separated role ids */
    unsigned roles;     /* packed role list, read with model_role_next() */
    unsigned emojis;    /* packed custom emoji list, read with model_emoji_next() */
    unsigned stickers;  /* packed sticker list, read with model_sticker_next() */
    int folder;         /* index in model_t.folders, -1 when not in a folder */
} guild_t;

/* A server folder from the user's settings; its guilds are consecutive in the list. */
typedef struct {
    char id[24];
    unsigned name;      /* 0 if unnamed */
    unsigned color;     /* 0xRRGGBB */
    int has_color;
} folder_t;

typedef struct {
    char id[24];
    char parent[24];    /* category */
    long long position;
    unsigned name;
    unsigned topic;     /* 0 if none */
    int type;
    /* Direct messages: the other user (or the group icon, with user_id empty). */
    char user_id[24];
    char avatar[40];
    /* Read state: unread when last_message > read, mentions counts pings (and DMs). */
    char last_message[24];
    char read[24];
    int mentions;
    int muted;
    long long mute_until;
    int notify;         /* NOTIFY_*: NOTIFY_DEFAULT follows the category, then the server */
} channel_t;

typedef struct {
    sb_t strings;
    guild_t *guilds;
    unsigned nguilds;
    channel_t *channels;
    unsigned nchannels;
    unsigned dm_first;  /* direct messages, most recent first */
    unsigned dm_count;
    char user_id[24];
    char user_avatar[40];
    unsigned user_name;
    int premium;        /* Nitro tier, 0 without: stickers of other servers need it */
    /* User settings, synced with the other clients. */
    char status[16];    /* "online", "idle", "dnd" or "invisible" */
    unsigned custom_status; /* its text, emoji first; 0 if none */
    int developer_mode; /* "Copy ID" in the menus */
    folder_t *folders;
    unsigned nfolders;
} model_t;

/* Builds the model from the READY payload `d`. Never returns NULL. */
model_t *model_from_ready(json_t d);
void model_free(model_t *m);

/*
 * Applies a gateway event (CHANNEL_CREATE/UPDATE/DELETE, THREAD_CREATE/UPDATE/DELETE
 * for threads we are in, GUILD_CREATE/UPDATE/DELETE,
 * GUILD_ROLE_CREATE/UPDATE/DELETE, GUILD_EMOJIS_UPDATE, USER_SETTINGS_UPDATE, GUILD_MEMBER_UPDATE for
 * us). Returns a new model, or NULL when nothing changed.
 * `m` is left untouched; read state carries over by channel id.
 */
model_t *model_apply(const model_t *m, const char *event, json_t d);

/* What notifies in channel i: NOTIFY_ALL, NOTIFY_MENTIONS or NOTIFY_NOTHING. DMs notify for everything. */
int model_notify(const model_t *m, unsigned i);
/* Channel i is muted at `now_ms`, itself, through its category or through its server. */
int model_muted(const model_t *m, unsigned i, long long now_ms);
/* Server g is muted at `now_ms`. */
int model_guild_muted(const model_t *m, int g, long long now_ms);

static __inline int model_is_thread(int type)
{
    return type == CH_NEWS_THREAD || type == CH_PUBLIC_THREAD || type == CH_PRIVATE_THREAD;
}

/* Snowflake order: negative, zero or positive like strcmp. */
int model_id_cmp(const char *a, const char *b);
int model_find_channel(const model_t *m, const char *id);
int model_find_guild(const model_t *m, const char *id);
/* Index of the guild owning channel i, -1 for direct messages. */
int model_channel_guild(const model_t *m, unsigned i);
int model_unread(const model_t *m, unsigned i);
typedef struct {
    char id[24];
    unsigned color;     /* 0xRRGGBB, 0 for none */
    int position;
    int hoist;          /* shown apart in the member list */
    const char *name;
    int name_len;
} model_role_t;

/* Iterates the roles of guild g: start with *cursor = 0; returns 0 at the end. */
int model_role_next(const model_t *m, int g, unsigned *cursor, model_role_t *out);
typedef struct {
    char id[24];
    int animated;
    int format;         /* stickers: 1 PNG, 2 APNG, 3 Lottie, 4 GIF */
    const char *name;
    int name_len;
} model_emoji_t;

/* Iterates the custom emoji of guild g: start with *cursor = 0; returns 0 at the end. */
int model_emoji_next(const model_t *m, int g, unsigned *cursor, model_emoji_t *out);
/* Same for the guild's stickers. */
int model_sticker_next(const model_t *m, int g, unsigned *cursor, model_emoji_t *out);

/* Color of the highest colored role among `roles` (comma-separated ids), 0 if none. */
unsigned model_role_color(const model_t *m, int g, const char *roles);

/* Whether we have role `role_id` in guild g. */
int model_has_role(const model_t *m, int g, const char *role_id);

static __inline const char *model_str(const model_t *m, unsigned off)
{
    return m->strings.data + off;
}
