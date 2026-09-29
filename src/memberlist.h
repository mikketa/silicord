#pragma once
#include "json.h"
#include "sb.h"

/*
 * A server's member list, as Discord streams it after a channel subscription
 * (op 14): GUILD_MEMBER_LIST_UPDATE carries SYNC, INSERT, UPDATE, DELETE and
 * INVALIDATE operations on a flat list of group headers and members.
 */

enum { ML_UNKNOWN, ML_OFFLINE, ML_ONLINE, ML_IDLE, ML_DND };

typedef struct {
    int valid;          /* 0 for slots we have not received */
    int group;          /* 1 for a group header */
    char id[24];        /* group: "online", "offline" or a role id; member: user id */
    int count;          /* group: members in it */
    sb_t name;          /* member: nickname, else display name, else username */
    char avatar[48];    /* the user's own avatar */
    char member_avatar[48]; /* member: the avatar for this server, empty if none */
    sb_t roles;         /* comma-separated role ids */
    int status;         /* ML_* */
    sb_t activity;      /* custom status text or "Playing ..." */
    int bot;
} ml_item_t;

typedef struct {
    char guild[24];
    char list_id[32];   /* "everyone" or a hash of the channel's permissions */
    ml_item_t *items;
    int n, cap;
    /* Size of each group, from the "groups" field. */
    struct {
        char id[24];
        int count;
    } groups[64];
    int ngroups;
} ml_t;

/* Applies one GUILD_MEMBER_LIST_UPDATE. A different guild or list id starts over. Returns 1 when it changed. */
int ml_apply(ml_t *l, json_t d);
void ml_free(ml_t *l);
/* Status of a presence object's "status" string. */
int ml_status(json_t status);
/* Text shown under a name for a presence object: custom status, else "Playing ..." and the like. */
void ml_activity(json_t presence, sb_t *out);
/* Members in group `id`, 0 if unknown. */
int ml_group_count(const ml_t *l, const char *id);
