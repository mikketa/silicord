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
    int id;
    sb_t text;           /* with its emoji, if any, in front */
    int count;
    int me;
} msg_answer_t;

typedef struct {
    sb_t question;
    msg_answer_t *answers;
    int nanswers;
    int multi;           /* several answers allowed */
    int final;
    long long expiry_ms; /* Unix ms, 0 if none */
} msg_poll_t;

/* A bot's button or select menu. */
enum { COMP_BUTTON = 2, COMP_STRING_SELECT = 3, COMP_USER_SELECT = 5, COMP_ROLE_SELECT = 6,
       COMP_MENTIONABLE_SELECT = 7, COMP_CHANNEL_SELECT = 8 };
enum { BUTTON_PRIMARY = 1, BUTTON_SECONDARY, BUTTON_SUCCESS, BUTTON_DANGER, BUTTON_LINK, BUTTON_PREMIUM };

typedef struct {
    int type;            /* COMP_* */
    int style;           /* buttons: BUTTON_* */
    int row;             /* components of one row sit side by side */
    int disabled;
    sb_t label;          /* a button's label, or a select's placeholder */
    sb_t emoji;          /* unicode emoji, or a custom emoji's name */
    char emoji_id[24];
    sb_t custom_id;
    sb_t url;            /* link buttons */
    sb_t options;        /* string selects: "label\tvalue\n" per option */
} msg_component_t;

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
    char reply_id[24];
    int system;      /* join notices and other non-user messages */
    int deleted;
    int edited;
    int pinned;
    msg_file_t *files;
    int nfiles;
    msg_embed_t *embeds;
    int nembeds;
    msg_reaction_t *reactions;
    int nreactions;
    msg_poll_t *poll;    /* NULL if the message has none */
    msg_component_t *components;
    int ncomponents;
    char app_id[24];     /* the bot's application, for component interactions */
    int flags;           /* message flags (64: only we see it) */
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
    int first_new;       /* first unread one: the "NEW" line goes above it */
    int mentions_me;     /* highlighted */
    int mention_everyone;
} msg_t;

enum { BATCH_HISTORY, BATCH_OLDER, BATCH_NEW, BATCH_UPDATE, BATCH_DELETE, BATCH_REACTION, BATCH_PINS, BATCH_SEARCH,
       BATCH_POLL_VOTE };

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
    /* BATCH_HISTORY around a message: its id, to jump to it. BATCH_SEARCH: total results. */
    char around[24];
    int total;
} msg_batch_t;

int msg_parse(json_t obj, msg_t *out);
void msg_free(msg_t *m);
/* Frees files, embeds, reactions and the sticker, leaving author, text and reply. */
void msg_free_extras(msg_t *m);
void msg_embed_free(msg_embed_t *e);
void msg_poll_free(msg_poll_t *p);
void msg_components_free(msg_t *m);

/* The REST API returns newest first; the batch is reversed to oldest first. */
msg_batch_t *msg_batch_from_array(json_t arr, int kind, const char *channel_id, int limit);
msg_batch_t *msg_batch_one(json_t obj, int kind);
void msg_batch_free(msg_batch_t *b);
/* MESSAGE_REACTION_ADD / _REMOVE; `me` is our user id. */
msg_batch_t *msg_batch_reaction(json_t d, int delta, const char *me);
/* A search response: {total_results, messages: [[hit, context...], ...]}; keeps the hits, best first. */
msg_batch_t *msg_batch_search(json_t root);
/* MESSAGE_POLL_VOTE_ADD / _REMOVE: msgs[0].id is the message, `total` the answer id. */
msg_batch_t *msg_batch_poll_vote(json_t d, int delta, const char *me);
/* The emoji of a reaction as the REST API wants it in a URL: "%F0%9F%91%8D" or "name:id". */
void msg_reaction_path(const msg_reaction_t *r, sb_t *out);

/* "2026-09-28T12:34:56.789+00:00" in milliseconds since the Unix epoch, 0 if it is not a valid date. */
long long msg_iso_ms(const char *iso);

/* Milliseconds since the Unix epoch encoded in a snowflake. */
long long snowflake_ms(const char *id);
