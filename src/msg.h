#pragma once
#include "json.h"
#include "sb.h"

/* A file sent with a message. `url` is a full https URL; images use the resizing media proxy. */
typedef struct {
    sb_t url;
    sb_t name;
    long long size;
    int width, height;   /* images and videos */
    int image;           /* displayable inline */
    int spoiler;
} msg_file_t;

typedef struct {
    sb_t name;
    sb_t value;
    int inline_;
} msg_field_t;

/* A link preview or bot embed. */
typedef struct {
    int has_color;
    unsigned color;      /* 0xRRGGBB, left bar */
    int media_only;      /* image, gifv or video preview without text: shown like an attachment */
    sb_t provider, author, title, url, description, footer;
    sb_t image;          /* full https URL */
    int image_w, image_h;
    sb_t thumbnail;
    int thumb_w, thumb_h;
    msg_field_t *fields;
    int nfields;
} msg_embed_t;

typedef struct {
    char emoji_id[24];   /* custom emoji, empty for a unicode one */
    sb_t emoji;          /* the unicode emoji, or the custom emoji's name */
    int count;
    int me;              /* we reacted */
} msg_reaction_t;

/* A chat message, flattened for display. Content is plain UTF-8 with mentions resolved. */
typedef struct {
    char id[24];
    char channel_id[24];
    char author_id[24];
    char avatar[40];
    sb_t author;
    sb_t text;
    sb_t content;    /* raw content as sent, for editing */
    sb_t reply;      /* "name: first line" of the message replied to, or empty */
    int system;      /* join notices and other non-user messages */
    int deleted;
    int edited;
    msg_file_t *files;
    int nfiles;
    msg_embed_t *embeds;
    int nembeds;
    msg_reaction_t *reactions;
    int nreactions;
    char sticker_id[24];
    int sticker_format;  /* 1 png, 2 apng, 3 lottie, 4 gif */
    sb_t sticker_name;
    sb_t member_roles;   /* comma-separated, gateway messages in servers */
    int has_member;
    /* Layout cache and view state, owned by the UI. */
    void *ui;
    int revealed;    /* spoilers shown */
    int height;
    int height_w;
    int grouped;
} msg_t;

enum { BATCH_HISTORY, BATCH_OLDER, BATCH_NEW, BATCH_UPDATE, BATCH_DELETE, BATCH_REACTION };

typedef struct {
    int kind;
    char channel_id[24];
    char before[24];   /* for BATCH_OLDER: the message the request started from */
    msg_t *msgs;       /* oldest first */
    int n;
    int has_more;      /* history: older messages may exist */
    int status;        /* HTTP status when a request failed, 0 otherwise */
    /* BATCH_REACTION: msgs[0] holds the message id and one reaction (count unused). */
    int delta;         /* +1 added, -1 removed */
    int mine;          /* by us */
} msg_batch_t;

int msg_parse(json_t obj, msg_t *out);
void msg_free(msg_t *m);
/* Frees files, embeds, reactions and the sticker, leaving author, text and reply. */
void msg_free_extras(msg_t *m);
void msg_embed_free(msg_embed_t *e);

/* The REST API returns newest first; the batch is reversed to oldest first. */
msg_batch_t *msg_batch_from_array(json_t arr, int kind, const char *channel_id, int limit);
msg_batch_t *msg_batch_one(json_t obj, int kind);
void msg_batch_free(msg_batch_t *b);
/* MESSAGE_REACTION_ADD / _REMOVE; `me` is our user id. */
msg_batch_t *msg_batch_reaction(json_t d, int delta, const char *me);
/* The emoji of a reaction as the REST API wants it in a URL: "%F0%9F%91%8D" or "name:id". */
void msg_reaction_path(const msg_reaction_t *r, sb_t *out);

/* Milliseconds since the Unix epoch encoded in a snowflake. */
long long snowflake_ms(const char *id);
