#pragma once
#include "json.h"
#include "sb.h"

/* A chat message, flattened for display. Content is plain UTF-8 with mentions resolved. */
typedef struct {
    char id[24];
    char channel_id[24];
    char author_id[24];
    char avatar[40];
    sb_t author;
    sb_t text;
    sb_t reply;      /* "name: first line" of the message replied to, or empty */
    int system;      /* join notices and other non-user messages */
    int deleted;
    /* Layout cache and view state, owned by the UI. */
    void *ui;
    int revealed;    /* spoilers shown */
    int height;
    int height_w;
    int grouped;
} msg_t;

enum { BATCH_HISTORY, BATCH_OLDER, BATCH_NEW, BATCH_UPDATE, BATCH_DELETE };

typedef struct {
    int kind;
    char channel_id[24];
    char before[24];   /* for BATCH_OLDER: the message the request started from */
    msg_t *msgs;       /* oldest first */
    int n;
    int has_more;      /* history: older messages may exist */
    int status;        /* HTTP status when a request failed, 0 otherwise */
} msg_batch_t;

int msg_parse(json_t obj, msg_t *out);
void msg_free(msg_t *m);

/* The REST API returns newest first; the batch is reversed to oldest first. */
msg_batch_t *msg_batch_from_array(json_t arr, int kind, const char *channel_id, int limit);
msg_batch_t *msg_batch_one(json_t obj, int kind);
void msg_batch_free(msg_batch_t *b);

/* Milliseconds since the Unix epoch encoded in a snowflake. */
long long snowflake_ms(const char *id);
