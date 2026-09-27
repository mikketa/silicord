#include "model.h"
#include "mem.h"

#define PERM_ADMINISTRATOR 0x8ull
#define PERM_VIEW_CHANNEL 0x400ull
#define MAX_ROLES 64

typedef struct {
    json_t json;
    char id[24];
    char parent[24];
    int type;
    long long position;
    int visible;
} tmp_channel_t;

/* ---- JSON helpers ---- */

/* READY for user sessions may nest guild fields under "properties". */
static int field(json_t obj, const char *key, json_t *out)
{
    json_t props;

    return json_get(obj, key, out) || (json_get(obj, "properties", &props) && json_get(props, key, out));
}

static int id_eq(json_t v, const char *id)
{
    char tmp[24];

    json_raw(v, tmp, sizeof tmp);
    for (int i = 0;; i++) {
        if (tmp[i] != id[i])
            return 0;
        if (!tmp[i])
            return 1;
    }
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static unsigned long long to_u64(json_t v)
{
    char tmp[24];
    unsigned long long n = 0;

    json_raw(v, tmp, sizeof tmp);
    for (const char *p = tmp; *p >= '0' && *p <= '9'; p++)
        n = n * 10 + (unsigned long long)(*p - '0');
    return n;
}

static long long to_i64(json_t v)
{
    long long n = 0;

    json_int(v, &n);
    return n;
}

static unsigned add_str(model_t *m, json_t v)
{
    unsigned off = (unsigned)m->strings.len;

    json_str(v, &m->strings);
    sb_addn(&m->strings, "", 1);
    return off;
}

/* Snowflakes compare as numbers: shorter is smaller. */
static int id_cmp(const char *a, const char *b)
{
    int la = 0, lb = 0;

    while (a[la])
        la++;
    while (b[lb])
        lb++;
    if (la != lb)
        return la - lb;
    for (int i = 0; i < la; i++)
        if (a[i] != b[i])
            return a[i] - b[i];
    return 0;
}

/* ---- Permissions ---- */

static int is_voice(int type)
{
    return type == CH_VOICE || type == CH_STAGE;
}

static int has_role(const char (*mine)[24], int nmine, json_t id)
{
    for (int i = 0; i < nmine; i++)
        if (id_eq(id, mine[i]))
            return 1;
    return 0;
}

static unsigned long long channel_perms(json_t ch, unsigned long long base, const char *guild_id,
                                        const char *user_id, const char (*mine)[24], int nmine)
{
    json_t ows, ow, v, type;
    json_iter_t it;
    unsigned long long perms = base, allow = 0, deny = 0;

    if (!json_get(ch, "permission_overwrites", &ows))
        return perms;
    /* @everyone, then all roles together, then the member. */
    json_iter(ows, &it);
    while (json_next(&it, NULL, &ow))
        if (json_get(ow, "id", &v) && id_eq(v, guild_id)) {
            perms &= ~(json_get(ow, "deny", &v) ? to_u64(v) : 0);
            perms |= json_get(ow, "allow", &v) ? to_u64(v) : 0;
        }
    json_iter(ows, &it);
    while (json_next(&it, NULL, &ow))
        if (json_get(ow, "id", &v) && !id_eq(v, guild_id) && has_role(mine, nmine, v)) {
            deny |= json_get(ow, "deny", &type) ? to_u64(type) : 0;
            allow |= json_get(ow, "allow", &type) ? to_u64(type) : 0;
        }
    perms = (perms & ~deny) | allow;
    json_iter(ows, &it);
    while (json_next(&it, NULL, &ow))
        if (json_get(ow, "id", &v) && id_eq(v, user_id)) {
            perms &= ~(json_get(ow, "deny", &v) ? to_u64(v) : 0);
            perms |= json_get(ow, "allow", &v) ? to_u64(v) : 0;
        }
    return perms;
}

/* Finds our role ids in merged_members[index] or in the guild's own member list. */
static int my_roles(json_t d, json_t guild, unsigned index, const char *user_id, char (*out)[24])
{
    json_t merged, list, v, roles, role, member = {0};
    json_iter_t it, rit;
    unsigned i = 0;
    int n = 0, found = 0;

    if (json_get(d, "merged_members", &merged)) {
        json_iter(merged, &it);
        while (json_next(&it, NULL, &list) && i++ < index)
            ;
        if (i == index + 1) {
            json_iter(list, &it);
            while (!found && json_next(&it, NULL, &member))
                found = json_get(member, "user_id", &v) && id_eq(v, user_id);
        }
    }
    if (!found && json_get(guild, "members", &list)) {
        json_iter(list, &it);
        while (!found && json_next(&it, NULL, &member)) {
            json_t user;
            found = (json_get(member, "user_id", &v) && id_eq(v, user_id)) ||
                    (json_get(member, "user", &user) && json_get(user, "id", &v) && id_eq(v, user_id));
        }
    }
    if (!found)
        return -1;
    if (json_get(member, "roles", &roles)) {
        json_iter(roles, &rit);
        while (n < MAX_ROLES && json_next(&rit, NULL, &role))
            json_raw(role, out[n++], 24);
    }
    return n;
}

/* ---- Ordering ---- */

static int channel_before(const tmp_channel_t *a, const tmp_channel_t *b)
{
    if (is_voice(a->type) != is_voice(b->type))
        return !is_voice(a->type);
    if (a->position != b->position)
        return a->position < b->position;
    return id_cmp(a->id, b->id) < 0;
}

static void sort_channels(tmp_channel_t **v, unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        tmp_channel_t *x = v[i];
        unsigned j = i;
        while (j > 0 && channel_before(x, v[j - 1])) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

static void push_channel(model_t *m, unsigned *cap, const tmp_channel_t *c)
{
    json_t name;
    channel_t *out;

    if (m->nchannels == *cap) {
        *cap = *cap ? *cap * 2 : 256;
        m->channels = mem_realloc(m->channels, *cap * sizeof *m->channels);
    }
    out = &m->channels[m->nchannels++];
    for (unsigned k = 0; k < sizeof out->id; k++)
        if (!(out->id[k] = c->id[k]))
            break;
    out->type = c->type;
    out->name = json_get(c->json, "name", &name) ? add_str(m, name) : 0;
}

static void add_guild_channels(model_t *m, unsigned *cap, json_t d, json_t g, unsigned index, guild_t *guild)
{
    json_t chans, ch, v, role, roles = {0};
    json_iter_t it;
    char mine[MAX_ROLES][24];
    unsigned long long base = 0;
    int nmine, known;
    unsigned n = 0, total, nlist = 0;
    tmp_channel_t *tmp;
    tmp_channel_t **list;

    guild->first = m->nchannels;
    guild->count = 0;
    if (!json_get(g, "channels", &chans))
        return;
    total = (unsigned)json_count(chans);
    if (!total)
        return;

    /* Base permissions: @everyone plus our roles; owners and admins see everything. */
    nmine = my_roles(d, g, index, m->user_id, mine);
    known = nmine >= 0 && json_get(g, "roles", &roles);
    if (known) {
        json_iter(roles, &it);
        while (json_next(&it, NULL, &role))
            if (json_get(role, "id", &v) && (id_eq(v, guild->id) || has_role((const char(*)[24])mine, nmine, v)))
                base |= json_get(role, "permissions", &v) ? to_u64(v) : 0;
        if ((field(g, "owner_id", &v) && id_eq(v, m->user_id)) || (base & PERM_ADMINISTRATOR))
            known = 0;
    }

    tmp = mem_alloc(total * sizeof *tmp);
    list = mem_alloc(total * sizeof *list);
    json_iter(chans, &it);
    while (n < total && json_next(&it, NULL, &ch)) {
        tmp_channel_t *c = &tmp[n++];
        c->json = ch;
        if (json_get(ch, "id", &v))
            json_raw(v, c->id, sizeof c->id);
        if (json_get(ch, "parent_id", &v))
            json_raw(v, c->parent, sizeof c->parent);
        c->type = json_get(ch, "type", &v) ? (int)to_i64(v) : 0;
        c->position = json_get(ch, "position", &v) ? to_i64(v) : 0;
        c->visible = !known ||
                     (channel_perms(ch, base, guild->id, m->user_id, (const char(*)[24])mine, nmine) & PERM_VIEW_CHANNEL);
    }

    /* Channels without a category first, then each category and its channels. */
    for (unsigned i = 0; i < n; i++)
        if (tmp[i].type != CH_CATEGORY && !tmp[i].parent[0] && tmp[i].visible)
            list[nlist++] = &tmp[i];
    sort_channels(list, nlist);
    for (unsigned i = 0; i < nlist; i++)
        push_channel(m, cap, list[i]);

    nlist = 0;
    for (unsigned i = 0; i < n; i++)
        if (tmp[i].type == CH_CATEGORY)
            list[nlist++] = &tmp[i];
    sort_channels(list, nlist);
    {
        unsigned ncat = nlist;
        tmp_channel_t **cats = mem_alloc((ncat + 1) * sizeof *cats);
        for (unsigned i = 0; i < ncat; i++)
            cats[i] = list[i];
        for (unsigned c = 0; c < ncat; c++) {
            nlist = 0;
            for (unsigned i = 0; i < n; i++)
                if (tmp[i].type != CH_CATEGORY && tmp[i].visible && str_eq(tmp[i].parent, cats[c]->id))
                    list[nlist++] = &tmp[i];
            if (!nlist)
                continue;
            sort_channels(list, nlist);
            push_channel(m, cap, cats[c]);
            for (unsigned i = 0; i < nlist; i++)
                push_channel(m, cap, list[i]);
        }
        mem_free(cats);
    }
    guild->count = m->nchannels - guild->first;
    mem_free(list);
    mem_free(tmp);
}

/* Rank of a guild in the user's sidebar order, -1 if unknown (new guilds go on top). */
static int guild_rank(json_t d, const char *id)
{
    json_t settings, folders, folder, ids, v;
    json_iter_t it, fit;
    int rank = 0;

    if (!json_get(d, "user_settings", &settings))
        return -1;
    if (json_get(settings, "guild_folders", &folders)) {
        json_iter(folders, &it);
        while (json_next(&it, NULL, &folder))
            if (json_get(folder, "guild_ids", &ids)) {
                json_iter(ids, &fit);
                while (json_next(&fit, NULL, &v)) {
                    if (id_eq(v, id))
                        return rank;
                    rank++;
                }
            }
    } else if (json_get(settings, "guild_positions", &ids)) {
        json_iter(ids, &fit);
        while (json_next(&fit, NULL, &v)) {
            if (id_eq(v, id))
                return rank;
            rank++;
        }
    }
    return -1;
}

model_t *model_from_ready(json_t d)
{
    model_t *m = mem_alloc(sizeof *m);
    json_t user, guilds, g, v;
    json_iter_t it;
    unsigned cap = 0, total, i = 0;
    int *rank;

    sb_addn(&m->strings, "", 1); /* offset 0 is the empty string */
    if (json_get(d, "user", &user)) {
        if (json_get(user, "id", &v))
            json_raw(v, m->user_id, sizeof m->user_id);
        if (json_get(user, "avatar", &v))
            json_raw(v, m->user_avatar, sizeof m->user_avatar);
        if ((json_get(user, "global_name", &v) && json_type(v) == JSON_STRING) || json_get(user, "username", &v))
            m->user_name = add_str(m, v);
    }
    if (!json_get(d, "guilds", &guilds))
        return m;

    total = (unsigned)json_count(guilds);
    m->guilds = mem_alloc((total + 1) * sizeof *m->guilds);
    rank = mem_alloc((total + 1) * sizeof *rank);
    json_iter(guilds, &it);
    while (json_next(&it, NULL, &g)) {
        guild_t *out = &m->guilds[m->nguilds];
        json_t name;

        if (!field(g, "name", &name)) {
            i++;
            continue; /* unavailable guild */
        }
        if (json_get(g, "id", &v))
            json_raw(v, out->id, sizeof out->id);
        if (field(g, "icon", &v))
            json_raw(v, out->icon, sizeof out->icon);
        out->name = add_str(m, name);
        add_guild_channels(m, &cap, d, g, i, out);
        rank[m->nguilds] = guild_rank(d, out->id);
        m->nguilds++;
        i++;
    }

    /* Stable insertion sort by sidebar rank. */
    for (unsigned a = 1; a < m->nguilds; a++) {
        guild_t x = m->guilds[a];
        int r = rank[a];
        unsigned b = a;
        while (b > 0 && rank[b - 1] > r) {
            m->guilds[b] = m->guilds[b - 1];
            rank[b] = rank[b - 1];
            b--;
        }
        m->guilds[b] = x;
        rank[b] = r;
    }
    mem_free(rank);
    return m;
}

void model_free(model_t *m)
{
    if (!m)
        return;
    sb_free(&m->strings);
    mem_free(m->guilds);
    mem_free(m->channels);
    mem_free(m);
}
