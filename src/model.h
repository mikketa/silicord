#pragma once
#include "json.h"
#include "sb.h"

/* Compact copy of what the UI needs from READY. Strings are offsets into `strings`. */

enum {
    CH_TEXT = 0,
    CH_VOICE = 2,
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
} guild_t;

typedef struct {
    char id[24];
    unsigned name;
    int type;
} channel_t;

typedef struct {
    sb_t strings;
    guild_t *guilds;
    unsigned nguilds;
    channel_t *channels;
    unsigned nchannels;
    char user_id[24];
    char user_avatar[40];
    unsigned user_name;
} model_t;

/* Builds the model from the READY payload `d`. Never returns NULL. */
model_t *model_from_ready(json_t d);
void model_free(model_t *m);

static __inline const char *model_str(const model_t *m, unsigned off)
{
    return m->strings.data + off;
}
