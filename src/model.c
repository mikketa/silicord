#include "model.h"
#include "mem.h"

#define PERM_ADMINISTRATOR 0x8ull
#define PERM_VIEW_CHANNEL 0x400ull
#define MAX_ROLES 64

typedef char role_id_t[24];

/* ---- JSON helpers ---- */

/* READY for user sessions may nest guild fields under "properties". */
static int field(json_t obj, const char *key, json_t *out)
{
    json_t props;

    return json_get(obj, key, out) || (json_get(obj, "properties", &props) && json_get(props, key, out));
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int id_eq(json_t v, const char *id)
{
    char tmp[24];

    json_raw(v, tmp, sizeof tmp);
    return str_eq(tmp, id);
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

static int is_true(json_t v)
{
    return json_type(v) == JSON_TRUE;
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

static void copy_id(char *dst, const char *src, size_t size)
{
    size_t i = 0;

    for (; i + 1 < size && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

/* ---- Roles and permissions ---- */

static int has_role(const role_id_t *mine, int n, json_t id)
{
    for (int i = 0; i < n; i++)
        if (id_eq(id, mine[i]))
            return 1;
    return 0;
}

static unsigned long long channel_perms(json_t ch, unsigned long long base, const char *guild_id,
                                        const char *user_id, const role_id_t *mine, int nmine)
{
    json_t ows, ow, v;
    json_iter_t it;
    unsigned long long perms = base, allow = 0, deny = 0;

    if (!json_get(ch, "permission_overwrites", &ows))
        return perms;
    /* @everyone, then all our roles together, then us. */
    json_iter(ows, &it);
    while (json_next(&it, NULL, &ow))
        if (json_get(ow, "id", &v) && id_eq(v, guild_id)) {
            perms &= ~(json_get(ow, "deny", &v) ? to_u64(v) : 0);
            perms |= json_get(ow, "allow", &v) ? to_u64(v) : 0;
        }
    json_iter(ows, &it);
    while (json_next(&it, NULL, &ow))
        if (json_get(ow, "id", &v) && !id_eq(v, guild_id) && has_role(mine, nmine, v)) {
            deny |= json_get(ow, "deny", &v) ? to_u64(v) : 0;
            allow |= json_get(ow, "allow", &v) ? to_u64(v) : 0;
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

static int read_roles(json_t list, role_id_t *out)
{
    json_iter_t it;
    json_t role;
    int n = 0;

    json_iter(list, &it);
    while (n < MAX_ROLES && json_next(&it, NULL, &role))
        json_raw(role, out[n++], sizeof out[0]);
    return n;
}

/* Our role ids from merged_members[index] or the guild's member list; -1 if we are not listed. */
static int my_roles(json_t d, json_t guild, unsigned index, const char *user_id, role_id_t *out)
{
    json_t merged, list, v, roles, member = {0};
    json_iter_t it;
    unsigned i = 0;
    int found = 0;

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
    return json_get(member, "roles", &roles) ? read_roles(roles, out) : 0;
}

static unsigned roles_string(model_t *m, const role_id_t *mine, int n)
{
    unsigned off;

    if (n <= 0)
        return 0;
    off = (unsigned)m->strings.len;
    for (int i = 0; i < n; i++) {
        if (i)
            sb_add(&m->strings, ",");
        sb_add(&m->strings, mine[i]);
    }
    sb_addn(&m->strings, "", 1);
    return off;
}

static int parse_roles(const model_t *m, unsigned off, role_id_t *out)
{
    const char *p = model_str(m, off);
    int n = 0;

    while (*p && n < MAX_ROLES) {
        size_t k = 0;
        while (*p && *p != ',') {
            if (k + 1 < sizeof out[0])
                out[n][k++] = *p;
            p++;
        }
        out[n++][k] = 0;
        if (*p == ',')
            p++;
    }
    return n;
}

int model_has_role(const model_t *m, int g, const char *role_id)
{
    role_id_t mine[MAX_ROLES];
    int n;

    if (g < 0 || (unsigned)g >= m->nguilds)
        return 0;
    n = parse_roles(m, m->guilds[g].my_roles, mine);
    for (int i = 0; i < n; i++)
        if (str_eq(mine[i], role_id))
            return 1;
    return 0;
}

/* ---- Role list ----
 * Packed in the string table, one role per line: "id color position hoist\tname\n".
 */

static void pack_role(sb_t *out, json_t role)
{
    json_t v, colors;
    char id[24] = "";
    long long color = 0, pos = 0;
    sb_t name = {0};

    if (!json_get(role, "id", &v))
        return;
    json_raw(v, id, sizeof id);
    if (json_get(role, "colors", &colors) && json_get(colors, "primary_color", &v))
        json_int(v, &color);
    else if (json_get(role, "color", &v))
        json_int(v, &color);
    if (json_get(role, "position", &v))
        json_int(v, &pos);
    if (json_get(role, "name", &v))
        json_str(v, &name);
    for (size_t i = 0; i < name.len; i++)
        if (name.data[i] == '\n' || name.data[i] == '\t')
            name.data[i] = ' ';
    sb_add(out, id);
    sb_add(out, " ");
    sb_i64(out, color);
    sb_add(out, " ");
    sb_i64(out, pos);
    sb_add(out, json_get(role, "hoist", &v) && is_true(v) ? " 1\t" : " 0\t");
    if (name.len)
        sb_addn(out, name.data, name.len);
    sb_add(out, "\n");
    sb_free(&name);
}

static unsigned pack_roles(model_t *m, json_t roles)
{
    sb_t packed = {0};
    json_iter_t it;
    json_t role;
    unsigned off;

    json_iter(roles, &it);
    while (json_next(&it, NULL, &role))
        pack_role(&packed, role);
    off = (unsigned)m->strings.len;
    if (packed.len)
        sb_addn(&m->strings, packed.data, packed.len);
    sb_addn(&m->strings, "", 1);
    sb_free(&packed);
    return off;
}

static long long parse_num(const char **p)
{
    long long n = 0;
    int neg = **p == '-';

    if (neg)
        (*p)++;
    while (**p >= '0' && **p <= '9')
        n = n * 10 + (*(*p)++ - '0');
    if (**p == ' ')
        (*p)++;
    return neg ? -n : n;
}

int model_role_next(const model_t *m, int g, unsigned *cursor, model_role_t *out)
{
    const char *base, *p, *line_end;
    int k = 0;

    if (g < 0 || (unsigned)g >= m->nguilds || !m->guilds[g].roles)
        return 0;
    base = m->strings.data + m->guilds[g].roles;
    p = base + *cursor;
    if (!*p)
        return 0;
    while (p[k] && p[k] != ' ' && k < (int)sizeof out->id - 1) {
        out->id[k] = p[k];
        k++;
    }
    out->id[k] = 0;
    p += k + (p[k] == ' ');
    out->color = (unsigned)parse_num(&p) & 0xFFFFFF;
    out->position = (int)parse_num(&p);
    out->hoist = *p == '1';
    while (*p && *p != '\t' && *p != '\n')
        p++;
    if (*p == '\t')
        p++;
    for (line_end = p; *line_end && *line_end != '\n'; line_end++)
        ;
    out->name = p;
    out->name_len = (int)(line_end - p);
    *cursor = (unsigned)(line_end - base) + (*line_end == '\n');
    return 1;
}

unsigned model_role_color(const model_t *m, int g, const char *roles)
{
    model_role_t r;
    unsigned cursor = 0, color = 0;
    int best = -1;

    while (model_role_next(m, g, &cursor, &r)) {
        size_t n = 0;
        const char *p = roles;
        if (!r.color || r.position <= best)
            continue;
        while (r.id[n])
            n++;
        /* Is r.id one of the comma-separated ids? */
        while (p && *p) {
            size_t k = 0;
            while (p[k] && p[k] != ',')
                k++;
            if (k == n) {
                size_t i = 0;
                while (i < n && p[i] == r.id[i])
                    i++;
                if (i == n) {
                    best = r.position;
                    color = r.color;
                    break;
                }
            }
            p += k + (p[k] == ',');
        }
    }
    return color;
}

/* ---- Custom emoji ----
 * Packed like roles, one per line: "id animated name\n". Unavailable ones are left out.
 */

static unsigned pack_emojis(model_t *m, json_t list)
{
    sb_t packed = {0};
    json_iter_t it;
    json_t e, v;
    unsigned off;

    json_iter(list, &it);
    while (json_next(&it, NULL, &e)) {
        char id[24] = "";
        sb_t name = {0};
        if (!json_get(e, "id", &v) || (json_get(e, "available", &v) && json_type(v) == JSON_FALSE))
            continue;
        json_get(e, "id", &v);
        json_raw(v, id, sizeof id);
        if (json_get(e, "name", &v))
            json_str(v, &name);
        if (!name.len) {
            sb_free(&name);
            continue;
        }
        sb_add(&packed, id);
        sb_add(&packed, json_get(e, "animated", &v) && is_true(v) ? " 1 " : " 0 ");
        sb_addn(&packed, name.data, name.len);
        sb_add(&packed, "\n");
        sb_free(&name);
    }
    off = (unsigned)m->strings.len;
    if (packed.len)
        sb_addn(&m->strings, packed.data, packed.len);
    sb_addn(&m->strings, "", 1);
    sb_free(&packed);
    return off;
}

int model_emoji_next(const model_t *m, int g, unsigned *cursor, model_emoji_t *out)
{
    const char *base, *p;
    int k = 0;

    if (g < 0 || (unsigned)g >= m->nguilds || !m->guilds[g].emojis)
        return 0;
    base = m->strings.data + m->guilds[g].emojis;
    p = base + *cursor;
    if (!*p)
        return 0;
    while (p[k] && p[k] != ' ' && k < (int)sizeof out->id - 1) {
        out->id[k] = p[k];
        k++;
    }
    out->id[k] = 0;
    p += k + (p[k] == ' ');
    out->animated = *p == '1';
    p += 2;
    out->name = p;
    while (*p && *p != '\n')
        p++;
    out->name_len = (int)(p - out->name);
    *cursor = (unsigned)(p - base) + (*p == '\n');
    return 1;
}

/* Base permissions: @everyone plus our roles. Owners and administrators see everything. */
static void guild_perms(model_t *m, guild_t *out, json_t g, const role_id_t *mine, int nmine)
{
    json_t role, v, roles = {0};
    json_iter_t it;

    out->base_perms = 0;
    out->my_roles = roles_string(m, mine, nmine);
    out->sees_all = nmine < 0 || !json_get(g, "roles", &roles);
    if (out->sees_all)
        return;
    json_iter(roles, &it);
    while (json_next(&it, NULL, &role))
        if (json_get(role, "id", &v) && (id_eq(v, out->id) || has_role(mine, nmine, v)))
            out->base_perms |= json_get(role, "permissions", &v) ? to_u64(v) : 0;
    if ((field(g, "owner_id", &v) && id_eq(v, m->user_id)) || (out->base_perms & PERM_ADMINISTRATOR))
        out->sees_all = 1;
}

static int visible(const model_t *m, const guild_t *g, json_t ch)
{
    role_id_t mine[MAX_ROLES];
    int n;

    if (g->sees_all)
        return 1;
    n = parse_roles(m, g->my_roles, mine);
    return (channel_perms(ch, g->base_perms, g->id, m->user_id, mine, n) & PERM_VIEW_CHANNEL) != 0;
}

/* ---- Channels and ordering ---- */

static int is_voice(int type)
{
    return type == CH_VOICE || type == CH_STAGE;
}

static channel_t make_channel(model_t *m, json_t ch)
{
    channel_t c = {0};
    json_t v;

    if (json_get(ch, "id", &v))
        json_raw(v, c.id, sizeof c.id);
    if (json_get(ch, "parent_id", &v))
        json_raw(v, c.parent, sizeof c.parent);
    if (json_get(ch, "last_message_id", &v))
        json_raw(v, c.last_message, sizeof c.last_message);
    c.type = json_get(ch, "type", &v) ? (int)to_i64(v) : 0;
    c.position = json_get(ch, "position", &v) ? to_i64(v) : 0;
    if (json_get(ch, "name", &v) && json_type(v) == JSON_STRING)
        c.name = add_str(m, v);
    return c;
}

static int before(const channel_t *a, const channel_t *b)
{
    if (is_voice(a->type) != is_voice(b->type))
        return !is_voice(a->type);
    if (a->position != b->position)
        return a->position < b->position;
    return id_cmp(a->id, b->id) < 0;
}

static void sort_ptrs(const channel_t **v, unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        const channel_t *x = v[i];
        unsigned j = i;
        while (j > 0 && before(x, v[j - 1])) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

/*
 * Display order: channels outside any category, then each category followed by
 * its channels. Categories are kept even when empty so later channels have a
 * home; the UI skips empty ones.
 */
static void order_slice(channel_t *v, unsigned n)
{
    const channel_t **roots = mem_alloc((n + 1) * sizeof *roots);
    const channel_t **cats = mem_alloc((n + 1) * sizeof *cats);
    const channel_t **kids = mem_alloc((n + 1) * sizeof *kids);
    channel_t *out = mem_alloc((n + 1) * sizeof *out);
    unsigned nr = 0, nc = 0, k = 0;

    for (unsigned i = 0; i < n; i++) {
        int in_cat = 0;
        if (v[i].type == CH_CATEGORY) {
            cats[nc++] = &v[i];
            continue;
        }
        for (unsigned j = 0; v[i].parent[0] && j < n && !in_cat; j++)
            in_cat = v[j].type == CH_CATEGORY && str_eq(v[j].id, v[i].parent);
        if (!in_cat)
            roots[nr++] = &v[i];
    }
    sort_ptrs(roots, nr);
    sort_ptrs(cats, nc);
    for (unsigned i = 0; i < nr; i++)
        out[k++] = *roots[i];
    for (unsigned c = 0; c < nc; c++) {
        unsigned nk = 0;
        out[k++] = *cats[c];
        for (unsigned i = 0; i < n; i++)
            if (v[i].type != CH_CATEGORY && v[i].parent[0] && str_eq(v[i].parent, cats[c]->id))
                kids[nk++] = &v[i];
        sort_ptrs(kids, nk);
        for (unsigned i = 0; i < nk; i++)
            out[k++] = *kids[i];
    }
    for (unsigned i = 0; i < k; i++)
        v[i] = out[i];
    mem_free(out);
    mem_free(kids);
    mem_free(cats);
    mem_free(roots);
}

static void push(model_t *m, unsigned *cap, const channel_t *c)
{
    if (m->nchannels == *cap) {
        *cap = *cap ? *cap * 2 : 256;
        m->channels = mem_realloc(m->channels, *cap * sizeof *m->channels);
    }
    m->channels[m->nchannels++] = *c;
}

/* Fills `out` and appends the guild's visible channels. from_ready: roles come from READY. */
static void build_guild(model_t *m, unsigned *cap, json_t d, json_t g, unsigned index, int from_ready, guild_t *out)
{
    json_t v, chans, ch;
    json_iter_t it;
    role_id_t mine[MAX_ROLES];
    int nmine;
    unsigned total, n = 0;
    channel_t *tmp;

    *out = (guild_t){0};
    if (json_get(g, "id", &v))
        json_raw(v, out->id, sizeof out->id);
    if (field(g, "icon", &v))
        json_raw(v, out->icon, sizeof out->icon);
    if (field(g, "name", &v))
        out->name = add_str(m, v);
    if (field(g, "roles", &v))
        out->roles = pack_roles(m, v);
    if (field(g, "emojis", &v))
        out->emojis = pack_emojis(m, v);
    nmine = my_roles(d, g, index, m->user_id, mine);
    if (nmine < 0 && !from_ready)
        nmine = 0; /* just joined: no roles yet */
    guild_perms(m, out, g, mine, nmine);

    out->first = m->nchannels;
    if (!json_get(g, "channels", &chans))
        return;
    total = (unsigned)json_count(chans);
    tmp = mem_alloc((total + 1) * sizeof *tmp);
    json_iter(chans, &it);
    while (n < total && json_next(&it, NULL, &ch)) {
        json_t type;
        int is_cat = json_get(ch, "type", &type) && to_i64(type) == CH_CATEGORY;
        if (is_cat || visible(m, out, ch))
            tmp[n++] = make_channel(m, ch);
    }
    order_slice(tmp, n);
    for (unsigned i = 0; i < n; i++)
        push(m, cap, &tmp[i]);
    out->count = n;
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

/* ---- Direct messages ---- */

/* Recipients come as full user objects, or as ids pointing into READY's "users". */
static int next_recipient(json_t d, json_iter_t *it, int by_id, json_t *user)
{
    json_t v, users, u, uid;
    json_iter_t uit;

    if (!by_id)
        return json_next(it, NULL, user);
    while (json_next(it, NULL, &v)) {
        char id[24];
        json_raw(v, id, sizeof id);
        if (!json_get(d, "users", &users))
            return 0;
        json_iter(users, &uit);
        while (json_next(&uit, NULL, &u))
            if (json_get(u, "id", &uid) && id_eq(uid, id)) {
                *user = u;
                return 1;
            }
    }
    return 0;
}

static void add_user_name(model_t *m, json_t user)
{
    json_t v;

    if ((json_get(user, "global_name", &v) && json_type(v) == JSON_STRING) || json_get(user, "username", &v))
        json_str(v, &m->strings);
}

/* Returns 0 if `ch` is not a DM or group DM. */
static int make_dm(model_t *m, json_t d, json_t ch, channel_t *out)
{
    json_t v, list, user;
    json_iter_t it;
    int by_id = 0, n = 0;

    *out = make_channel(m, ch);
    if (!json_get(ch, "type", &v))
        out->type = CH_DM;
    if (out->type != CH_DM && out->type != CH_GROUP_DM)
        return 0;
    if (out->type == CH_GROUP_DM && json_get(ch, "icon", &v))
        json_raw(v, out->avatar, sizeof out->avatar);
    if (out->name && model_str(m, out->name)[0])
        return 1; /* named group */

    /* Name after the recipients: "Ann" or "Ann, Bob, Carl". */
    out->name = (unsigned)m->strings.len;
    if (json_get(ch, "recipients", &list)) {
        json_iter(list, &it);
    } else if (json_get(ch, "recipient_ids", &list)) {
        by_id = 1;
        json_iter(list, &it);
    } else {
        it.p = it.end = NULL;
    }
    while (it.p && next_recipient(d, &it, by_id, &user)) {
        if (n++)
            sb_add(&m->strings, ", ");
        add_user_name(m, user);
        if (n == 1 && out->type == CH_DM) {
            if (json_get(user, "id", &v))
                json_raw(v, out->user_id, sizeof out->user_id);
            if (json_get(user, "avatar", &v))
                json_raw(v, out->avatar, sizeof out->avatar);
        }
    }
    if (!n)
        sb_add(&m->strings, "Unknown user");
    sb_addn(&m->strings, "", 1);
    return 1;
}

static void add_dms(model_t *m, unsigned *cap, json_t d)
{
    json_t list, ch;
    json_iter_t it;
    unsigned total, n = 0;
    channel_t *tmp;

    m->dm_first = m->nchannels;
    if (!json_get(d, "private_channels", &list))
        return;
    total = (unsigned)json_count(list);
    tmp = mem_alloc((total + 1) * sizeof *tmp);
    json_iter(list, &it);
    while (n < total && json_next(&it, NULL, &ch))
        if (make_dm(m, d, ch, &tmp[n]))
            n++;
    /* Most recent conversation first. */
    for (unsigned a = 1; a < n; a++) {
        channel_t x = tmp[a];
        unsigned b = a;
        while (b > 0 && id_cmp(tmp[b - 1].last_message, x.last_message) < 0) {
            tmp[b] = tmp[b - 1];
            b--;
        }
        tmp[b] = x;
    }
    for (unsigned i = 0; i < n; i++)
        push(m, cap, &tmp[i]);
    m->dm_count = n;
    mem_free(tmp);
}

/* ---- Lookups ---- */

int model_id_cmp(const char *a, const char *b)
{
    return id_cmp(a, b);
}

int model_find_channel(const model_t *m, const char *id)
{
    for (unsigned i = 0; i < m->nchannels; i++)
        if (str_eq(m->channels[i].id, id))
            return (int)i;
    return -1;
}

int model_find_guild(const model_t *m, const char *id)
{
    for (unsigned g = 0; g < m->nguilds; g++)
        if (str_eq(m->guilds[g].id, id))
            return (int)g;
    return -1;
}

int model_channel_guild(const model_t *m, unsigned i)
{
    for (unsigned g = 0; g < m->nguilds; g++)
        if (i >= m->guilds[g].first && i < m->guilds[g].first + m->guilds[g].count)
            return (int)g;
    return -1;
}

int model_unread(const model_t *m, unsigned i)
{
    const channel_t *c = &m->channels[i];

    if (c->type == CH_CATEGORY || c->type == CH_VOICE || c->type == CH_STAGE || c->type == CH_FORUM ||
        c->type == CH_MEDIA || !c->last_message[0] || !c->read[0])
        return c->mentions > 0;
    return id_cmp(c->last_message, c->read) > 0;
}

/* ---- Read state and mutes ---- */

/* READY sends these either as {"entries": [...]} or as a bare array. */
static int entries(json_t d, const char *key, json_t *out)
{
    json_t v;

    if (!json_get(d, key, &v))
        return 0;
    if (json_type(v) == JSON_OBJECT)
        return json_get(v, "entries", out);
    *out = v;
    return json_type(v) == JSON_ARRAY;
}

static void apply_read_state(model_t *m, json_t d)
{
    json_t list, e, v;
    json_iter_t it;
    char id[24];

    if (!entries(d, "read_state", &list))
        return;
    json_iter(list, &it);
    while (json_next(&it, NULL, &e)) {
        int i;
        if (!json_get(e, "id", &v))
            continue;
        json_raw(v, id, sizeof id);
        i = model_find_channel(m, id);
        if (i < 0)
            continue;
        if (json_get(e, "last_message_id", &v))
            json_raw(v, m->channels[i].read, sizeof m->channels[i].read);
        if (json_get(e, "mention_count", &v))
            m->channels[i].mentions = (int)to_i64(v);
    }
}

static void apply_mutes(model_t *m, json_t d)
{
    json_t list, e, v, overrides, o;
    json_iter_t it, oit;
    char id[24];

    if (!entries(d, "user_guild_settings", &list))
        return;
    json_iter(list, &it);
    while (json_next(&it, NULL, &e)) {
        int g;
        id[0] = 0;
        if (json_get(e, "guild_id", &v))
            json_raw(v, id, sizeof id);
        if (id[0] && json_get(e, "muted", &v) && is_true(v) && (g = model_find_guild(m, id)) >= 0)
            m->guilds[g].muted = 1;
        if (!json_get(e, "channel_overrides", &overrides))
            continue;
        json_iter(overrides, &oit);
        while (json_next(&oit, NULL, &o)) {
            int i;
            if (!json_get(o, "muted", &v) || !is_true(v) || !json_get(o, "channel_id", &v))
                continue;
            json_raw(v, id, sizeof id);
            i = model_find_channel(m, id);
            if (i >= 0)
                m->channels[i].muted = 1;
        }
    }
}

/* ---- READY ---- */

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

    total = json_get(d, "guilds", &guilds) ? (unsigned)json_count(guilds) : 0;
    m->guilds = mem_alloc((total + 1) * sizeof *m->guilds);
    rank = mem_alloc((total + 1) * sizeof *rank);
    if (total) {
        json_iter(guilds, &it);
        while (json_next(&it, NULL, &g)) {
            if (field(g, "name", &v)) { /* skip unavailable guilds */
                build_guild(m, &cap, d, g, i, 1, &m->guilds[m->nguilds]);
                rank[m->nguilds] = guild_rank(d, m->guilds[m->nguilds].id);
                m->nguilds++;
            }
            i++;
        }
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
    add_dms(m, &cap, d);
    apply_read_state(m, d);
    apply_mutes(m, d);
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

/* ---- Live updates ---- */

static model_t *clone_empty(const model_t *m, unsigned extra_guilds)
{
    model_t *n = mem_alloc(sizeof *n);

    sb_addn(&n->strings, m->strings.data, m->strings.len);
    copy_id(n->user_id, m->user_id, sizeof n->user_id);
    copy_id(n->user_avatar, m->user_avatar, sizeof n->user_avatar);
    n->user_name = m->user_name;
    n->guilds = mem_alloc((m->nguilds + extra_guilds + 1) * sizeof *n->guilds);
    return n;
}

static void copy_guild(model_t *n, unsigned *cap, const model_t *m, unsigned g, const char *skip_id)
{
    guild_t *out = &n->guilds[n->nguilds++];

    *out = m->guilds[g];
    out->first = n->nchannels;
    out->count = 0;
    for (unsigned i = m->guilds[g].first; i < m->guilds[g].first + m->guilds[g].count; i++)
        if (!skip_id || !str_eq(m->channels[i].id, skip_id)) {
            push(n, cap, &m->channels[i]);
            out->count++;
        }
}

/* Copies the DM list, replacing (or dropping, when `with` is NULL) the entry `id`; `front` goes on top. */
static void copy_dms(model_t *n, unsigned *cap, const model_t *m, const char *id, const channel_t *with,
                     const channel_t *front)
{
    n->dm_first = n->nchannels;
    if (front)
        push(n, cap, front);
    for (unsigned i = m->dm_first; i < m->dm_first + m->dm_count; i++) {
        if (id && str_eq(m->channels[i].id, id)) {
            if (with)
                push(n, cap, with);
        } else {
            push(n, cap, &m->channels[i]);
        }
    }
    n->dm_count = n->nchannels - n->dm_first;
}

/* Keeps what we knew about a channel (read state, mute) across an update. */
static void carry_state(channel_t *c, const model_t *m)
{
    int i = model_find_channel(m, c->id);

    if (i < 0)
        return;
    copy_id(c->read, m->channels[i].read, sizeof c->read);
    c->mentions = m->channels[i].mentions;
    c->muted = m->channels[i].muted;
    if (id_cmp(m->channels[i].last_message, c->last_message) > 0)
        copy_id(c->last_message, m->channels[i].last_message, sizeof c->last_message);
}

static model_t *apply_channel(const model_t *m, json_t d, int deleted)
{
    json_t v;
    char id[24] = {0}, guild_id[24] = {0};
    int old, gi = -1;
    unsigned cap = 0;
    model_t *n;

    if (json_get(d, "id", &v))
        json_raw(v, id, sizeof id);
    if (json_get(d, "guild_id", &v))
        json_raw(v, guild_id, sizeof guild_id);
    old = model_find_channel(m, id);
    if (guild_id[0] && (gi = model_find_guild(m, guild_id)) < 0)
        return NULL;
    if (deleted && old < 0)
        return NULL;

    if (gi < 0) {
        channel_t c;
        n = clone_empty(m, 0);
        for (unsigned g = 0; g < m->nguilds; g++)
            copy_guild(n, &cap, m, g, NULL);
        if (deleted) {
            copy_dms(n, &cap, m, id, NULL, NULL);
        } else if (!make_dm(n, d, d, &c)) {
            model_free(n);
            return NULL;
        } else {
            carry_state(&c, m);
            copy_dms(n, &cap, m, old >= 0 ? id : NULL, old >= 0 ? &c : NULL, old >= 0 ? NULL : &c);
        }
        return n;
    }

    n = clone_empty(m, 0);
    for (unsigned g = 0; g < m->nguilds; g++) {
        const guild_t *src = &m->guilds[g];
        channel_t *tmp;
        unsigned k = 0;
        int show;

        if ((int)g != gi) {
            copy_guild(n, &cap, m, g, NULL);
            continue;
        }
        tmp = mem_alloc((src->count + 2) * sizeof *tmp);
        for (unsigned i = src->first; i < src->first + src->count; i++)
            if (!str_eq(m->channels[i].id, id))
                tmp[k++] = m->channels[i];
        show = !deleted && ((json_get(d, "type", &v) && to_i64(v) == CH_CATEGORY) || visible(m, src, d));
        if (show) {
            tmp[k] = make_channel(n, d);
            carry_state(&tmp[k], m);
            k++;
        }
        order_slice(tmp, k);
        n->guilds[n->nguilds] = *src;
        n->guilds[n->nguilds].first = n->nchannels;
        n->guilds[n->nguilds].count = k;
        n->nguilds++;
        for (unsigned i = 0; i < k; i++)
            push(n, &cap, &tmp[i]);
        mem_free(tmp);
        if (!show && old < 0) {
            model_free(n); /* a channel we cannot see: nothing changes */
            return NULL;
        }
    }
    copy_dms(n, &cap, m, NULL, NULL, NULL);
    return n;
}

static model_t *apply_guild_create(const model_t *m, json_t d)
{
    json_t v;
    char id[24] = {0};
    int gi;
    unsigned cap = 0;
    guild_t fresh;
    model_t *n;
    json_t none = {0};

    if (json_get(d, "id", &v))
        json_raw(v, id, sizeof id);
    if (!field(d, "name", &v))
        return NULL; /* unavailable */
    gi = model_find_guild(m, id);
    n = clone_empty(m, 1);
    build_guild(n, &cap, none, d, 0, 0, &fresh);
    for (unsigned i = fresh.first; i < fresh.first + fresh.count; i++)
        carry_state(&n->channels[i], m);
    if (gi >= 0)
        fresh.muted = m->guilds[gi].muted;
    else
        n->guilds[n->nguilds++] = fresh; /* joined: on top, like Discord */
    for (unsigned g = 0; g < m->nguilds; g++) {
        if ((int)g == gi)
            n->guilds[n->nguilds++] = fresh;
        else
            copy_guild(n, &cap, m, g, NULL);
    }
    copy_dms(n, &cap, m, NULL, NULL, NULL);
    return n;
}

static model_t *apply_guild_patch(const model_t *m, const char *event, json_t d)
{
    json_t v, user;
    char id[24] = {0};
    int gi;
    unsigned cap = 0;
    model_t *n;

    if (json_get(d, str_eq(event, "GUILD_MEMBER_UPDATE") ? "guild_id" : "id", &v))
        json_raw(v, id, sizeof id);
    if ((gi = model_find_guild(m, id)) < 0)
        return NULL;
    if (str_eq(event, "GUILD_DELETE") && json_get(d, "unavailable", &v) && is_true(v))
        return NULL; /* outage, not a leave */
    if (str_eq(event, "GUILD_MEMBER_UPDATE") &&
        !(json_get(d, "user", &user) && json_get(user, "id", &v) && id_eq(v, m->user_id)))
        return NULL;

    n = clone_empty(m, 0);
    for (unsigned g = 0; g < m->nguilds; g++)
        if ((int)g != gi || !str_eq(event, "GUILD_DELETE"))
            copy_guild(n, &cap, m, g, NULL);
    copy_dms(n, &cap, m, NULL, NULL, NULL);
    if (str_eq(event, "GUILD_DELETE"))
        return n;

    {
        guild_t *g = &n->guilds[gi];
        role_id_t mine[MAX_ROLES];
        int nmine;

        if (str_eq(event, "GUILD_MEMBER_UPDATE")) {
            json_t roles;
            nmine = json_get(d, "roles", &roles) ? read_roles(roles, mine) : 0;
            g->my_roles = roles_string(n, mine, nmine);
            return n;
        }
        /* GUILD_UPDATE: name, icon, and permissions if roles came along. */
        if (field(d, "name", &v))
            g->name = add_str(n, v);
        if (field(d, "icon", &v))
            json_raw(v, g->icon, sizeof g->icon);
        if (json_get(d, "roles", &v)) {
            g->roles = pack_roles(n, v);
            nmine = parse_roles(n, g->my_roles, mine);
            guild_perms(n, g, d, mine, nmine);
        }
    }
    return n;
}

/* GUILD_ROLE_CREATE / UPDATE carry {guild_id, role}, GUILD_ROLE_DELETE {guild_id, role_id}. */
static model_t *apply_role(const model_t *m, json_t d, int deleted)
{
    json_t v, role = {0};
    char gid[24] = "", rid[24] = "";
    unsigned cap = 0, cursor = 0;
    model_role_t r;
    model_t *n;
    sb_t packed = {0};
    int gi;

    if (json_get(d, "guild_id", &v))
        json_raw(v, gid, sizeof gid);
    if ((gi = model_find_guild(m, gid)) < 0)
        return NULL;
    if (deleted) {
        if (json_get(d, "role_id", &v))
            json_raw(v, rid, sizeof rid);
    } else if (!json_get(d, "role", &role) || !json_get(role, "id", &v)) {
        return NULL;
    } else {
        json_raw(v, rid, sizeof rid);
    }
    /* Keep every other role line as it was, then add the new version. */
    while (model_role_next(m, gi, &cursor, &r)) {
        const char *line = m->strings.data + m->guilds[gi].roles, *start, *end;
        if (str_eq(r.id, rid))
            continue;
        for (start = r.name; start > line && start[-1] != '\n'; start--)
            ;
        end = r.name + r.name_len;
        sb_addn(&packed, start, (size_t)(end - start));
        sb_add(&packed, "\n");
    }
    if (!deleted)
        pack_role(&packed, role);

    n = clone_empty(m, 0);
    for (unsigned g = 0; g < m->nguilds; g++)
        copy_guild(n, &cap, m, g, NULL);
    copy_dms(n, &cap, m, NULL, NULL, NULL);
    n->guilds[gi].roles = (unsigned)n->strings.len;
    if (packed.len)
        sb_addn(&n->strings, packed.data, packed.len);
    sb_addn(&n->strings, "", 1);
    sb_free(&packed);
    return n;
}

static model_t *apply_emojis(const model_t *m, json_t d)
{
    json_t v, list;
    char gid[24] = "";
    unsigned cap = 0;
    int gi;
    model_t *n;

    if (json_get(d, "guild_id", &v))
        json_raw(v, gid, sizeof gid);
    if ((gi = model_find_guild(m, gid)) < 0 || !json_get(d, "emojis", &list))
        return NULL;
    n = clone_empty(m, 0);
    for (unsigned g = 0; g < m->nguilds; g++)
        copy_guild(n, &cap, m, g, NULL);
    copy_dms(n, &cap, m, NULL, NULL, NULL);
    n->guilds[gi].emojis = pack_emojis(n, list);
    return n;
}

model_t *model_apply(const model_t *m, const char *event, json_t d)
{
    if (str_eq(event, "GUILD_EMOJIS_UPDATE"))
        return apply_emojis(m, d);
    if (str_eq(event, "GUILD_ROLE_CREATE") || str_eq(event, "GUILD_ROLE_UPDATE"))
        return apply_role(m, d, 0);
    if (str_eq(event, "GUILD_ROLE_DELETE"))
        return apply_role(m, d, 1);
    if (str_eq(event, "CHANNEL_CREATE") || str_eq(event, "CHANNEL_UPDATE"))
        return apply_channel(m, d, 0);
    if (str_eq(event, "CHANNEL_DELETE"))
        return apply_channel(m, d, 1);
    if (str_eq(event, "GUILD_CREATE"))
        return apply_guild_create(m, d);
    if (str_eq(event, "GUILD_UPDATE") || str_eq(event, "GUILD_DELETE") || str_eq(event, "GUILD_MEMBER_UPDATE"))
        return apply_guild_patch(m, event, d);
    return NULL;
}
