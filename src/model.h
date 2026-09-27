#pragma once
#include "json.h"
#include "sb.h"

/* Compact copy of what the UI needs from READY. Strings are offsets into `strings`. */

enum {
    CH_TEXT = 0,
    CH_DM = 1,
    CH_VOICE = 2,
    CH_GROUP_DM = 3,
    CH_CATEGORY = 4,
    CH_NEWS = 5,
    CH_STAGE = 13,
    CH_FORUM = 15,
    CH_MEDIA = 16,
};

typedef struct {
    char id[24];
    char icon[40];      /* CDN hash, empty if none */
    unsigned name;
    unsigned first;     /* first channel, channels are stored in display order */
    unsigned count;
    int muted;
} guild_t;

typedef struct {
    char id[24];
    unsigned name;
    int type;
    /* Direct messages: the other user (or the group icon, with user_id empty). */
    char user_id[24];
    char avatar[40];
    /* Read state: unread when last_message > read, mentions counts pings (and DMs). */
    char last_message[24];
    char read[24];
    int mentions;
    int muted;
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
} model_t;

/* Builds the model from the READY payload `d`. Never returns NULL. */
model_t *model_from_ready(json_t d);
void model_free(model_t *m);

/* Snowflake order: negative, zero or positive like strcmp. */
int model_id_cmp(const char *a, const char *b);
int model_find_channel(const model_t *m, const char *id);
/* Index of the guild owning channel i, -1 for direct messages. */
int model_channel_guild(const model_t *m, unsigned i);
int model_unread(const model_t *m, unsigned i);

static __inline const char *model_str(const model_t *m, unsigned off)
{
    return m->strings.data + off;
}
