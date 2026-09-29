#include "model.h"
#include "mem.h"
#include "msg.h"
#include "sc_asm.h"

#define PERM_ADMINISTRATOR 0x8ull
#define PERM_VIEW_CHANNEL 0x400ull
#define MAX_ROLES 64

typedef char role_id_t[24];

/* ---- JSON helpers ---- */

/* READY for user sessions may nest guild fields under "properties". One pass over obj, which can be big. */
static int field(json_t obj, const char *key, json_t *out)
{
    json_iter_t it;
    json_t k, v, props = {0};

    if (json_type(obj) != JSON_OBJECT)
        return 0;
    json_iter(obj, &it);
    while (json_next(&it, &k, &v)) {
        if (json_str_eq(k, key)) {
            *out = v;
            return 1;
        }
        if (!props.p && json_str_eq(k, "properties"))
            props = v;
    }
    return json_get(props, key, out);
}

/* json_get of n keys in one pass over obj: vals[i] is keys[i]'s value, empty (p NULL) if it is missing. */
static void get_keys(json_t obj, const char *const *keys, json_t *vals, int n)
{
    json_iter_t it;
    json_t k, v;

    for (int i = 0; i < n; i++)
        vals[i] = (json_t){0};
    if (json_type(obj) != JSON_OBJECT)
        return;
    json_iter(obj, &it);
    while (json_next(&it, &k, &v))
        for (int i = 0; i < n; i++)
            if (!vals[i].p && json_str_eq(k, keys[i]))
                vals[i] = v; /* the first one, as json_get finds */
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int str_eq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
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

/*
 * Permission overwrites, packed per channel as "id allow deny\n" lines. They are
 * kept for every channel, the hidden ones too, so that a change of our roles
 * or of the permissions shows or hides channels without asking Discord.
 */
static unsigned pack_overwrites(model_t *m, json_t ows)
{
    json_t ow, v;
    json_iter_t it;
    unsigned off;

    if (json_type(ows) != JSON_ARRAY || !json_count(ows))
        return 0;
    off = (unsigned)m->strings.len;
    json_iter(ows, &it);
    while (json_next(&it, NULL, &ow)) {
        char id[24] = "";
        if (!json_get(ow, "id", &v))
            continue;
        json_raw(v, id, sizeof id);
        sb_add(&m->strings, id);
        sb_add(&m->strings, " ");
        sb_u64(&m->strings, json_get(ow, "allow", &v) ? to_u64(v) : 0);
        sb_add(&m->strings, " ");
        sb_u64(&m->strings, json_get(ow, "deny", &v) ? to_u64(v) : 0);
        sb_add(&m->strings, "\n");
    }
    sb_addn(&m->strings, "", 1);
    return off;
}

static unsigned long long read_u64(const char **p)
{
    unsigned long long n = 0;

    while (**p >= '0' && **p <= '9')
        n = n * 10 + (unsigned long long)(*(*p)++ - '0');
    if (**p == ' ')
        (*p)++;
    return n;
}

/* The next packed overwrite; 0 at the end. */
static int overwrite_next(const char **p, char *id, unsigned long long *allow, unsigned long long *deny)
{
    size_t k = 0;

    if (!**p)
        return 0;
    while (**p && **p != ' ') {
        if (k + 1 < 24)
            id[k++] = **p;
        (*p)++;
    }
    id[k] = 0;
    if (**p == ' ')
        (*p)++;
    *allow = read_u64(p);
    *deny = read_u64(p);
    while (**p && **p != '\n')
        (*p)++;
    if (**p == '\n')
        (*p)++;
    return 1;
}

static int in_roles(const role_id_t *mine, int n, const char *id)
{
    for (int i = 0; i < n; i++)
        if (str_eq(mine[i], id))
            return 1;
    return 0;
}

/* Discord's order: the @everyone overwrite, then all our roles' together, then ours. */
static unsigned long long channel_perms(const model_t *m, const channel_t *c, const guild_t *g,
                                        const role_id_t *mine, int nmine)
{
    unsigned long long perms = g->base_perms, allow, deny, roles_allow = 0, roles_deny = 0;
    const char *base = m->strings.data + c->overwrites, *p;
    char id[24];

    if (!c->overwrites)
        return perms;
    for (p = base; overwrite_next(&p, id, &allow, &deny);)
        if (str_eq(id, g->id))
            perms = (perms & ~deny) | allow;
    for (p = base; overwrite_next(&p, id, &allow, &deny);)
        if (!str_eq(id, g->id) && in_roles(mine, nmine, id)) {
            roles_deny |= deny;
            roles_allow |= allow;
        }
    perms = (perms & ~roles_deny) | roles_allow;
    for (p = base; overwrite_next(&p, id, &allow, &deny);)
        if (str_eq(id, m->user_id))
            perms = (perms & ~deny) | allow;
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

/*
 * Our role ids from READY's merged_members[index] (`merged`, empty outside READY)
 * or the guild's member list; -1 if we are not listed.
 */
static int my_roles(json_t merged, json_t guild, unsigned index, const char *user_id, role_id_t *out)
{
    json_t list, v, roles, member = {0};
    json_iter_t it;
    unsigned i = 0;
    int found = 0;

    if (merged.p) {
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
 * Packed in the string table, one role per line: "id color position permissions\tname\n".
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
    sb_add(out, " ");
    sb_u64(out, json_get(role, "permissions", &v) ? to_u64(v) : 0);
    sb_add(out, "\t");
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
    int neg = **p == '-';
    unsigned long long n;

    if (neg)
        (*p)++;
    n = read_u64(p); /* unsigned: -9223372036854775808 has no positive long long */
    return neg ? (long long)(0ull - n) : (long long)n;
}

static int role_next(const char *base, unsigned *cursor, model_role_t *out)
{
    const char *p = base + *cursor, *line_end;
    int k = 0;

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
    out->permissions = read_u64(&p);
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

int model_role_next(const model_t *m, int g, unsigned *cursor, model_role_t *out)
{
    if (g < 0 || (unsigned)g >= m->nguilds || !m->guilds[g].roles)
        return 0;
    return role_next(m->strings.data + m->guilds[g].roles, cursor, out);
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

/* ---- Custom emoji and stickers ----
 * Packed like roles, one per line: "id flag name\n", where the flag is 1 for an
 * animated emoji, or a sticker's format_type. Unavailable ones are left out.
 */

static int packed_next(const char *base, unsigned *cursor, model_emoji_t *out);

static unsigned pack_emojis(model_t *m, json_t list, int stickers)
{
    sb_t packed = {0};
    json_iter_t it;
    json_t e, v;
    unsigned off;

    json_iter(list, &it);
    while (json_next(&it, NULL, &e)) {
        char id[24] = "", flag[4] = " 0 ";
        sb_t name = {0};
        long long format = 0;
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
        for (size_t i = 0; i < name.len; i++)
            if (name.data[i] == '\n')
                name.data[i] = ' '; /* it would end the line */
        if (stickers && json_get(e, "format_type", &v) && json_int(v, &format) && format > 0 && format < 10)
            flag[1] = (char)('0' + format);
        else if (!stickers && json_get(e, "animated", &v) && is_true(v))
            flag[1] = '1';
        sb_add(&packed, id);
        sb_add(&packed, flag);
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
    if (g < 0 || (unsigned)g >= m->nguilds || !m->guilds[g].emojis)
        return 0;
    return packed_next(m->strings.data + m->guilds[g].emojis, cursor, out);
}

int model_sticker_next(const model_t *m, int g, unsigned *cursor, model_emoji_t *out)
{
    if (g < 0 || (unsigned)g >= m->nguilds || !m->guilds[g].stickers)
        return 0;
    return packed_next(m->strings.data + m->guilds[g].stickers, cursor, out);
}

static int packed_next(const char *base, unsigned *cursor, model_emoji_t *out)
{
    const char *p;
    int k = 0;

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
    out->format = *p - '0';
    if (*p)
        p++;
    if (*p == ' ')
        p++;
    out->name = p;
    while (*p && *p != '\n')
        p++;
    out->name_len = (int)(p - out->name);
    *cursor = (unsigned)(p - base) + (*p == '\n');
    return 1;
}

/*
 * Base permissions, @everyone's and our roles', from the packed role list.
 * Owners and administrators see everything, and so does anyone while their
 * roles are unknown.
 */
static void compute_perms(const model_t *m, guild_t *g)
{
    role_id_t mine[MAX_ROLES];
    int n = parse_roles(m, g->my_roles, mine);
    unsigned cursor = 0;
    model_role_t r;

    g->base_perms = 0;
    g->sees_all = !g->roles_known || !g->roles;
    if (g->sees_all)
        return;
    while (role_next(m->strings.data + g->roles, &cursor, &r))
        if (str_eq(r.id, g->id) || in_roles(mine, n, r.id))
            g->base_perms |= r.permissions;
    if (g->owner || (g->base_perms & PERM_ADMINISTRATOR))
        g->sees_all = 1;
}

/* ---- Channels and ordering ---- */

static int is_voice(int type)
{
    return type == CH_VOICE || type == CH_STAGE;
}

static channel_t make_channel(model_t *m, json_t ch)
{
    enum { C_ID, C_PARENT, C_LAST, C_TYPE, C_POSITION, C_NAME, C_TOPIC, C_OVERWRITES, C_COUNT };
    static const char *const keys[C_COUNT] = {"id",       "parent_id", "last_message_id", "type",
                                              "position", "name",      "topic",           "permission_overwrites"};
    channel_t c = {0};
    json_t k[C_COUNT];

    /* READY has thousands of channels: one pass over each. Missing keys read as empty or 0. */
    get_keys(ch, keys, k, C_COUNT);
    json_raw(k[C_ID], c.id, sizeof c.id);
    json_raw(k[C_PARENT], c.parent, sizeof c.parent);
    json_raw(k[C_LAST], c.last_message, sizeof c.last_message);
    c.type = (int)to_i64(k[C_TYPE]);
    c.position = to_i64(k[C_POSITION]);
    if (json_type(k[C_NAME]) == JSON_STRING)
        c.name = add_str(m, k[C_NAME]);
    if (json_type(k[C_TOPIC]) == JSON_STRING && k[C_TOPIC].end - k[C_TOPIC].p > 2)
        c.topic = add_str(m, k[C_TOPIC]);
    c.overwrites = pack_overwrites(m, k[C_OVERWRITES]);
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
/* Sorts a guild's channels for display; threads whose channel is missing are dropped. Returns the count. */
static unsigned order_slice(channel_t *v, unsigned n)
{
    const channel_t **roots = mem_alloc((n + 1) * sizeof *roots);
    const channel_t **cats = mem_alloc((n + 1) * sizeof *cats);
    const channel_t **kids = mem_alloc((n + 1) * sizeof *kids);
    channel_t *out = mem_alloc((n + 1) * sizeof *out);
    /* Each channel is placed once, even if a broken payload repeats an id. */
    unsigned char *placed = mem_alloc(n + 1);
    unsigned nr = 0, nc = 0, k = 0;

    for (unsigned i = 0; i < n; i++) {
        int in_cat = 0;
        if (model_is_thread(v[i].type))
            continue; /* placed under their channel below */
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
#define PLACE(ch)                                                                   \
    do {                                                                            \
        placed[(ch) - v] = 1;                                                       \
        out[k++] = *(ch);                                                           \
    } while (0)
#define WITH_THREADS(ch)                                                            \
    do {                                                                            \
        PLACE(ch);                                                                  \
        for (unsigned t_ = 0; t_ < n; t_++)                                         \
            if (!placed[t_] && model_is_thread(v[t_].type) && str_eq(v[t_].parent, (ch)->id)) \
                PLACE(&v[t_]);                                                      \
    } while (0)
    for (unsigned i = 0; i < nr; i++)
        WITH_THREADS(roots[i]);
    for (unsigned c = 0; c < nc; c++) {
        unsigned nk = 0;
        PLACE(cats[c]);
        for (unsigned i = 0; i < n; i++)
            if (!placed[i] && v[i].type != CH_CATEGORY && !model_is_thread(v[i].type) && v[i].parent[0] &&
                str_eq(v[i].parent, cats[c]->id)) {
                placed[i] = 1;
                kids[nk++] = &v[i];
            }
        sort_ptrs(kids, nk);
        for (unsigned i = 0; i < nk; i++) {
            placed[kids[i] - v] = 0; /* claimed above so a repeated category does not take it again */
            WITH_THREADS(kids[i]);
        }
    }
#undef WITH_THREADS
#undef PLACE
    for (unsigned i = 0; i < k; i++)
        v[i] = out[i];
    mem_free(placed);
    mem_free(out);
    mem_free(kids);
    mem_free(cats);
    mem_free(roots);
    return k;
}

static void push(model_t *m, unsigned *cap, const channel_t *c)
{
    if (m->nchannels == *cap) {
        *cap = *cap ? *cap * 2 : 256;
        m->channels = mem_realloc(m->channels, *cap * sizeof *m->channels);
    }
    m->channels[m->nchannels++] = *c;
}

/* The capacity follows the count: 16, then doubling at each power of two. */
static void push_hidden(model_t *m, const channel_t *c)
{
    unsigned n = m->nhidden;

    if (n == 0 || (n >= 16 && !(n & (n - 1))))
        m->hidden = mem_realloc(m->hidden, (n ? n * 2 : 16) * sizeof *m->hidden);
    m->hidden[m->nhidden++] = *c;
}

/*
 * Appends guild g's channels, `all` of them (k), shown and hidden: categories
 * always show, channels by our permissions, threads when their channel shows.
 * Sets g's slices of the channel and hidden lists.
 */
static void place_channels(model_t *m, unsigned *cap, guild_t *g, channel_t *all, unsigned k)
{
    role_id_t mine[MAX_ROLES];
    int nmine = parse_roles(m, g->my_roles, mine);
    unsigned char *show = mem_alloc(k + 1);
    channel_t *vis = mem_alloc((k + 1) * sizeof *vis);
    unsigned nvis = 0;

    for (unsigned i = 0; i < k; i++)
        if (!model_is_thread(all[i].type))
            show[i] = all[i].type == CH_CATEGORY || g->sees_all ||
                      (channel_perms(m, &all[i], g, mine, nmine) & PERM_VIEW_CHANNEL) != 0;
    for (unsigned i = 0; i < k; i++)
        if (model_is_thread(all[i].type))
            for (unsigned j = 0; j < k && !show[i]; j++)
                show[i] = show[j] && !model_is_thread(all[j].type) && all[j].type != CH_CATEGORY &&
                          str_eq(all[j].id, all[i].parent);
    g->hidden_first = m->nhidden;
    for (unsigned i = 0; i < k; i++) {
        if (show[i])
            vis[nvis++] = all[i];
        else
            push_hidden(m, &all[i]);
    }
    g->hidden_count = m->nhidden - g->hidden_first;
    nvis = order_slice(vis, nvis);
    g->first = m->nchannels;
    for (unsigned i = 0; i < nvis; i++)
        push(m, cap, &vis[i]);
    g->count = nvis;
    mem_free(vis);
    mem_free(show);
}

/* Guild g's channels, shown and hidden, without `skip_id`; *k gets the count. Free with mem_free. */
static channel_t *all_channels(const model_t *m, unsigned g, const char *skip_id, unsigned extra, unsigned *k)
{
    const guild_t *gd = &m->guilds[g];
    channel_t *all = mem_alloc((gd->count + gd->hidden_count + extra + 1) * sizeof *all);

    *k = 0;
    for (unsigned i = gd->first; i < gd->first + gd->count; i++)
        if (!skip_id || !str_eq(m->channels[i].id, skip_id))
            all[(*k)++] = m->channels[i];
    for (unsigned i = gd->hidden_first; i < gd->hidden_first + gd->hidden_count; i++)
        if (!skip_id || !str_eq(m->hidden[i].id, skip_id))
            all[(*k)++] = m->hidden[i];
    return all;
}

/* ---- Threads we joined ---- */

static int joined_has(const model_t *m, const char *id)
{
    const char *p = m->strings.data + m->joined;

    while (m->joined && *p) {
        size_t k = 0;
        while (p[k] && p[k] != '\n' && p[k] == id[k])
            k++;
        if (p[k] == '\n' && !id[k])
            return 1;
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    return 0;
}

/* Rewrites the list with `id` added or removed. */
static void joined_set(model_t *m, const char *id, int member)
{
    sb_t list = {0};
    const char *p = m->joined ? m->strings.data + m->joined : "";

    if (!id[0] || joined_has(m, id) == member)
        return;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n')
            e++;
        if (member || (size_t)(e - p) != sc_strlen(id) || !str_eq_n(p, id, (size_t)(e - p)))
            sb_addn(&list, p, (size_t)(e - p + (*e == '\n')));
        p = e + (*e == '\n');
    }
    if (member) {
        sb_add(&list, id);
        sb_add(&list, "\n");
    }
    m->joined = (unsigned)m->strings.len;
    if (list.len)
        sb_addn(&m->strings, list.data, list.len);
    sb_addn(&m->strings, "", 1);
    sb_free(&list);
}

/*
 * Fills `out` and appends the guild's channels, the ones we cannot see to the
 * hidden list. from_ready: roles come from READY, `merged` is its merged_members.
 * Joined threads are recorded.
 */
static void build_guild(model_t *m, unsigned *cap, json_t merged, json_t g, unsigned index, int from_ready,
                        guild_t *out)
{
    enum { G_ID, G_CHANNELS, G_THREADS, G_PROPS, G_ICON, G_NAME, G_ROLES, G_EMOJIS, G_NOTIFY, G_STICKERS, G_OWNER, G_COUNT };
    static const char *const keys[G_COUNT] = {"id", "channels", "threads", "properties", "icon", "name", "roles",
                                              "emojis", "default_message_notifications", "stickers", "owner_id"};
    json_t k[G_COUNT], ch;
    json_iter_t it;
    role_id_t mine[MAX_ROLES];
    int nmine;
    unsigned total, n = 0;
    channel_t *all;

    /* A guild with its channels is big: one pass over its keys, then field()'s fallback to "properties". */
    get_keys(g, keys, k, G_COUNT);
    for (int i = G_ICON; i < G_COUNT; i++)
        if (!k[i].p)
            json_get(k[G_PROPS], keys[i], &k[i]);
    *out = (guild_t){0};
    out->folder = -1;
    out->rank = -1;
    if (k[G_ID].p)
        json_raw(k[G_ID], out->id, sizeof out->id);
    if (k[G_ICON].p)
        json_raw(k[G_ICON], out->icon, sizeof out->icon);
    if (k[G_NAME].p)
        out->name = add_str(m, k[G_NAME]);
    if (k[G_ROLES].p)
        out->roles = pack_roles(m, k[G_ROLES]);
    if (k[G_EMOJIS].p)
        out->emojis = pack_emojis(m, k[G_EMOJIS], 0);
    out->default_notify = k[G_NOTIFY].p && to_i64(k[G_NOTIFY]) == 1 ? NOTIFY_MENTIONS : NOTIFY_ALL;
    if (k[G_STICKERS].p)
        out->stickers = pack_emojis(m, k[G_STICKERS], 1);
    out->owner = k[G_OWNER].p && id_eq(k[G_OWNER], m->user_id);
    nmine = my_roles(merged, g, index, m->user_id, mine);
    if (nmine < 0 && !from_ready)
        nmine = 0; /* just joined: no roles yet */
    out->roles_known = nmine >= 0;
    out->my_roles = roles_string(m, mine, nmine);
    compute_perms(m, out);

    total = (unsigned)json_count(k[G_CHANNELS]) + (unsigned)json_count(k[G_THREADS]);
    all = mem_alloc((total + 1) * sizeof *all);
    json_iter(k[G_CHANNELS], &it);
    while (n < total && json_next(&it, NULL, &ch))
        all[n++] = make_channel(m, ch);
    /* Active threads we joined; they show when their channel does. */
    json_iter(k[G_THREADS], &it);
    while (n < total && json_next(&it, NULL, &ch)) {
        json_t member, meta, arch;
        if (!json_get(ch, "member", &member))
            continue;
        all[n] = make_channel(m, ch);
        joined_set(m, all[n].id, 1);
        if (!(json_get(ch, "thread_metadata", &meta) && json_get(meta, "archived", &arch) && is_true(arch)))
            n++;
    }
    place_channels(m, cap, out, all, n);
    mem_free(all);
}

/* ---- Server order and folders ----
 * From the user settings' guild_folders (or older guild_positions), kept as
 * JSON text in the model so that servers joined later find their place.
 */

static void keep_folder_json(model_t *m, json_t settings)
{
    json_t v;

    if (!json_get(settings, "guild_folders", &v) && !json_get(settings, "guild_positions", &v))
        return;
    m->folder_json = (unsigned)m->strings.len;
    sb_add(&m->strings, json_get(settings, "guild_folders", &v) ? "{\"guild_folders\":" : "{\"guild_positions\":");
    sb_addn(&m->strings, v.p, (size_t)(v.end - v.p));
    sb_addn(&m->strings, "}", 2);
}

/* A copy of the kept JSON (strings may move while it is read), parsed into *out. */
static int folder_settings(const model_t *m, sb_t *copy, json_t *out)
{
    if (!m->folder_json)
        return 0;
    sb_add(copy, m->strings.data + m->folder_json);
    return json_parse(copy->data, copy->len, out);
}

/* Folder of guild `id` (only real folders, with an id), -1 if none; new folders are added to m. */
static int guild_folder(model_t *m, const char *id)
{
    json_t settings, folders, folder, ids, v;
    json_iter_t it, fit;
    sb_t copy = {0};
    int found = -1;

    if (!folder_settings(m, &copy, &settings) || !json_get(settings, "guild_folders", &folders)) {
        sb_free(&copy);
        return -1;
    }
    json_iter(folders, &it);
    while (found < 0 && json_next(&it, NULL, &folder)) {
        char fid[24] = "";
        if (!json_get(folder, "id", &v) || json_type(v) == JSON_NULL || !json_get(folder, "guild_ids", &ids))
            continue;
        json_raw(v, fid, sizeof fid);
        json_iter(ids, &fit);
        while (found < 0 && json_next(&fit, NULL, &v))
            if (id_eq(v, id)) {
                /* Known folder, or a new entry. */
                for (unsigned f = 0; f < m->nfolders && found < 0; f++)
                    if (str_eq(m->folders[f].id, fid))
                        found = (int)f;
                if (found >= 0)
                    break;
                m->folders = mem_realloc(m->folders, (m->nfolders + 1) * sizeof *m->folders);
                m->folders[m->nfolders] = (folder_t){0};
                copy_id(m->folders[m->nfolders].id, fid, sizeof m->folders[0].id);
                if (json_get(folder, "name", &v) && json_type(v) == JSON_STRING && v.end - v.p > 2)
                    m->folders[m->nfolders].name = add_str(m, v);
                if (json_get(folder, "color", &v) && json_type(v) == JSON_NUMBER) {
                    m->folders[m->nfolders].color = (unsigned)to_i64(v) & 0xFFFFFF;
                    m->folders[m->nfolders].has_color = 1;
                }
                found = (int)m->nfolders++;
            }
    }
    sb_free(&copy);
    return found;
}

/* Rank of a guild in the user's sidebar order, -1 if unknown (new guilds go on top). */
static int guild_rank(const model_t *m, const char *id)
{
    json_t settings, folders, folder, ids, v;
    json_iter_t it, fit;
    sb_t copy = {0};
    int rank = 0, found = -1;

    if (!folder_settings(m, &copy, &settings)) {
        sb_free(&copy);
        return -1;
    }
    if (json_get(settings, "guild_folders", &folders)) {
        json_iter(folders, &it);
        while (found < 0 && json_next(&it, NULL, &folder))
            if (json_get(folder, "guild_ids", &ids)) {
                json_iter(ids, &fit);
                while (found < 0 && json_next(&fit, NULL, &v))
                    found = id_eq(v, id) ? rank : (rank++, -1);
            }
    } else if (json_get(settings, "guild_positions", &ids)) {
        json_iter(ids, &fit);
        while (found < 0 && json_next(&fit, NULL, &v))
            found = id_eq(v, id) ? rank : (rank++, -1);
    }
    sb_free(&copy);
    return found;
}

/* Stable insertion sort by rank (-1, unknown, first). */
static void sort_guilds(guild_t *g, unsigned n)
{
    for (unsigned a = 1; a < n; a++) {
        guild_t x = g[a];
        unsigned b = a;
        while (b > 0 && g[b - 1].rank > x.rank) {
            g[b] = g[b - 1];
            b--;
        }
        g[b] = x;
    }
}

/* ---- Direct messages ---- */

/* The user objects recipient ids point to (READY's "users"), their ids read once. Free v with mem_free. */
typedef struct {
    char id[24];
    json_t user;
} user_ref_t;

typedef struct {
    user_ref_t *v;
    unsigned n;
} users_t;

static void users_read(json_t list, users_t *out)
{
    json_iter_t it;
    json_t u, uid;

    out->n = 0;
    out->v = mem_alloc((json_count(list) + 1) * sizeof *out->v);
    json_iter(list, &it);
    while (json_next(&it, NULL, &u))
        if (json_get(u, "id", &uid)) {
            json_raw(uid, out->v[out->n].id, sizeof out->v[0].id);
            out->v[out->n++].user = u;
        }
}

/* Recipients come as full user objects, or as ids pointing into `users`. */
static int next_recipient(const users_t *users, json_iter_t *it, int by_id, json_t *user)
{
    json_t v;

    if (!by_id)
        return json_next(it, NULL, user);
    while (json_next(it, NULL, &v)) {
        char id[24];
        json_raw(v, id, sizeof id);
        for (unsigned i = 0; i < users->n; i++)
            if (str_eq(users->v[i].id, id)) {
                *user = users->v[i].user;
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
static int make_dm(model_t *m, const users_t *users, json_t ch, channel_t *out)
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
    while (it.p && next_recipient(users, &it, by_id, &user)) {
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

static const char *dm_key(const channel_t *c)
{
    return c->last_message[0] ? c->last_message : c->id;
}

/* READY's private_channels, whose recipient ids point into its `user_list`. */
static void add_dms(model_t *m, unsigned *cap, json_t list, json_t user_list)
{
    json_t ch;
    json_iter_t it;
    unsigned total, n = 0;
    channel_t *tmp;
    users_t users;

    m->dm_first = m->nchannels;
    if (!list.p)
        return;
    total = (unsigned)json_count(list);
    tmp = mem_alloc((total + 1) * sizeof *tmp);
    users_read(user_list, &users);
    json_iter(list, &it);
    while (n < total && json_next(&it, NULL, &ch))
        if (make_dm(m, &users, ch, &tmp[n]))
            n++;
    mem_free(users.v);
    /* Most recent conversation first; one without messages by when it was created, like Discord. */
    for (unsigned a = 1; a < n; a++) {
        channel_t x = tmp[a];
        unsigned b = a;
        while (b > 0 && id_cmp(dm_key(&tmp[b - 1]), dm_key(&x)) < 0) {
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

/* A channel shown or hidden, by id, so state set while hidden is there once it shows. */
static channel_t *find_any(model_t *m, const char *id)
{
    int i = model_find_channel(m, id);

    if (i >= 0)
        return &m->channels[i];
    for (unsigned h = 0; h < m->nhidden; h++)
        if (str_eq(m->hidden[h].id, id))
            return &m->hidden[h];
    return NULL;
}

/*
 * The same lookup through a hash table, for the loops that look up every read
 * state or notification override: READY has thousands of each, and of channels.
 * Build it once the channels are in place; they must not move while it is used.
 */
typedef struct {
    model_t *m;
    unsigned *slot;     /* 1 + the channel's index, the hidden ones after the shown ones; 0 when empty */
    unsigned mask;
} chan_index_t;

static unsigned id_hash(const char *id)
{
    unsigned h = 2166136261u; /* FNV-1a */

    while (*id)
        h = (h ^ (unsigned char)*id++) * 16777619u;
    return h;
}

static channel_t *index_at(const chan_index_t *ix, unsigned slot)
{
    unsigned i = ix->slot[slot] - 1;

    return i < ix->m->nchannels ? &ix->m->channels[i] : &ix->m->hidden[i - ix->m->nchannels];
}

static void index_build(chan_index_t *ix, model_t *m)
{
    unsigned total = m->nchannels + m->nhidden, size = 16;

    while (size < 2 * total)
        size *= 2;
    ix->m = m;
    ix->mask = size - 1;
    ix->slot = mem_alloc(size * sizeof *ix->slot);
    for (unsigned i = 0; i < total; i++) {
        const char *id = i < m->nchannels ? m->channels[i].id : m->hidden[i - m->nchannels].id;
        unsigned h = id_hash(id) & ix->mask;
        while (ix->slot[h] && !str_eq(index_at(ix, h)->id, id))
            h = (h + 1) & ix->mask;
        if (!ix->slot[h])
            ix->slot[h] = i + 1; /* a repeated id keeps its first channel, as find_any finds */
    }
}

static channel_t *index_find(const chan_index_t *ix, const char *id)
{
    for (unsigned h = id_hash(id) & ix->mask; ix->slot[h]; h = (h + 1) & ix->mask)
        if (str_eq(index_at(ix, h)->id, id))
            return index_at(ix, h);
    return NULL;
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

/* READY sends its read states and settings either as {"entries": [...]} or as a bare array; v is empty if missing. */
static int entries(json_t v, json_t *out)
{
    if (!v.p)
        return 0;
    if (json_type(v) == JSON_OBJECT)
        return json_get(v, "entries", out);
    *out = v;
    return json_type(v) == JSON_ARRAY;
}

/* One read state: false when its channel is unknown. */
static int read_entry(const chan_index_t *ix, const char *id, json_t last, json_t mentions)
{
    channel_t *c = index_find(ix, id);

    if (!c)
        return 0;
    if (last.p)
        json_raw(last, c->read, sizeof c->read);
    if (mentions.p)
        c->mentions = (int)to_i64(mentions);
    return 1;
}

/* READY's read_state. Those of unknown channels are kept when some servers are unavailable: they may be theirs. */
static void apply_read_state(model_t *m, json_t reads)
{
    json_t list, e, v, last, mentions;
    json_iter_t it;
    char id[24];
    sb_t orphans = {0};
    chan_index_t ix;

    if (!entries(reads, &list))
        return;
    index_build(&ix, m);
    json_iter(list, &it);
    while (json_next(&it, NULL, &e)) {
        if (!json_get(e, "id", &v))
            continue;
        json_raw(v, id, sizeof id);
        last = mentions = (json_t){0};
        json_get(e, "last_message_id", &last);
        json_get(e, "mention_count", &mentions);
        if (!read_entry(&ix, id, last, mentions) && m->pending) {
            char raw[24] = "";
            json_raw(last, raw, sizeof raw);
            sb_add(&orphans, id);
            sb_add(&orphans, " ");
            sb_add(&orphans, raw[0] ? raw : "-");
            sb_add(&orphans, " ");
            sb_i64(&orphans, mentions.p ? to_i64(mentions) : 0);
            sb_add(&orphans, "\n");
        }
    }
    mem_free(ix.slot);
    if (orphans.len) {
        m->pending_reads = (unsigned)m->strings.len;
        sb_addn(&m->strings, orphans.data, orphans.len + 1);
    }
    sb_free(&orphans);
}

/* ---- Notification settings ---- */

/* Discord's message_notifications (0 all, 1 mentions, 2 nothing, 3 inherit) as NOTIFY_*. */
static int notify_level(json_t obj)
{
    json_t v;
    long long n = 3;

    if (json_get(obj, "message_notifications", &v))
        json_int(v, &n);
    return n >= 0 && n <= 2 ? (int)n + NOTIFY_ALL : NOTIFY_DEFAULT;
}

/* "muted" with its mute_config: when a timed mute ends. */
static int read_mute(json_t obj, long long *until)
{
    json_t v, cfg;
    sb_t iso = {0};

    *until = 0;
    if (!json_get(obj, "muted", &v) || !is_true(v))
        return 0;
    if (json_get(obj, "mute_config", &cfg) && json_type(cfg) == JSON_OBJECT && json_get(cfg, "end_time", &v) &&
        json_type(v) == JSON_STRING && json_str(v, &iso) && iso.len)
        *until = msg_iso_ms(iso.data);
    sb_free(&iso);
    return 1;
}

/*
 * One user_guild_settings entry, into the model `ix` indexes; guild_id null holds
 * the DM overrides. Replaces what was there.
 */
static void apply_settings_entry(const chan_index_t *ix, json_t e)
{
    model_t *m = ix->m;
    json_t v, overrides, o;
    json_iter_t it;
    char id[24] = "";
    int g = -1;
    unsigned first, count;

    if (json_get(e, "guild_id", &v) && json_type(v) == JSON_STRING) {
        json_raw(v, id, sizeof id);
        if ((g = model_find_guild(m, id)) < 0)
            return; /* an unavailable server's are kept with it at READY */
    }
    if (g >= 0) {
        guild_t *gd = &m->guilds[g];
        gd->muted = read_mute(e, &gd->mute_until);
        gd->notify = notify_level(e);
        gd->suppress_everyone = json_get(e, "suppress_everyone", &v) && is_true(v);
        gd->suppress_roles = json_get(e, "suppress_roles", &v) && is_true(v);
        first = gd->first;
        count = gd->count;
    } else {
        first = m->dm_first;
        count = m->dm_count;
    }
    for (unsigned i = first; i < first + count; i++) {
        m->channels[i].muted = 0;
        m->channels[i].mute_until = 0;
        m->channels[i].notify = NOTIFY_DEFAULT;
    }
    for (unsigned i = g >= 0 ? m->guilds[g].hidden_first : 0; g >= 0 && i < m->guilds[g].hidden_first + m->guilds[g].hidden_count; i++) {
        m->hidden[i].muted = 0;
        m->hidden[i].mute_until = 0;
        m->hidden[i].notify = NOTIFY_DEFAULT;
    }
    if (!json_get(e, "channel_overrides", &overrides))
        return;
    json_iter(overrides, &it);
    while (json_next(&it, NULL, &o)) {
        channel_t *c;
        if (!json_get(o, "channel_id", &v))
            continue;
        json_raw(v, id, sizeof id);
        if (!(c = index_find(ix, id)))
            continue;
        c->muted = read_mute(o, &c->mute_until);
        c->notify = notify_level(o);
    }
}

/* READY's user_guild_settings. */
static void apply_mutes(model_t *m, json_t settings)
{
    json_t list, e;
    json_iter_t it;
    chan_index_t ix;

    if (!entries(settings, &list))
        return;
    index_build(&ix, m);
    json_iter(list, &it);
    while (json_next(&it, NULL, &e))
        apply_settings_entry(&ix, e);
    mem_free(ix.slot);
}

static int mute_active(int muted, long long until, long long now_ms)
{
    return muted && (!until || until > now_ms);
}

static int parent_index(const model_t *m, unsigned i)
{
    return m->channels[i].parent[0] ? model_find_channel(m, m->channels[i].parent) : -1;
}

int model_guild_muted(const model_t *m, int g, long long now_ms)
{
    return g >= 0 && (unsigned)g < m->nguilds && mute_active(m->guilds[g].muted, m->guilds[g].mute_until, now_ms);
}

/* A channel, then its parent, then the parent's parent (thread, channel, category). */
int model_muted(const model_t *m, unsigned i, long long now_ms)
{
    for (int k = (int)i, depth = 0; k >= 0 && depth < 3; k = parent_index(m, (unsigned)k), depth++)
        if (mute_active(m->channels[k].muted, m->channels[k].mute_until, now_ms))
            return 1;
    return model_guild_muted(m, model_channel_guild(m, i), now_ms);
}

int model_notify(const model_t *m, unsigned i)
{
    int g = model_channel_guild(m, i);

    if (g < 0)
        return NOTIFY_ALL;
    for (int k = (int)i, depth = 0; k >= 0 && depth < 3; k = parent_index(m, (unsigned)k), depth++)
        if (m->channels[k].notify != NOTIFY_DEFAULT)
            return m->channels[k].notify;
    if (m->guilds[g].notify != NOTIFY_DEFAULT)
        return m->guilds[g].notify;
    return m->guilds[g].default_notify ? m->guilds[g].default_notify : NOTIFY_ALL;
}

/* ---- User settings ---- */

/* Status, custom status and developer mode from user_settings (or a USER_SETTINGS_UPDATE). */
static void read_user_settings(model_t *m, json_t s)
{
    json_t v, custom;

    if (json_get(s, "status", &v) && json_type(v) == JSON_STRING)
        json_raw(v, m->status, sizeof m->status);
    if (json_get(s, "developer_mode", &v))
        m->developer_mode = is_true(v);
    if (json_get(s, "custom_status", &custom)) {
        sb_t text = {0};
        m->custom_status = 0;
        if (json_type(custom) == JSON_OBJECT) {
            if (json_get(custom, "emoji_name", &v) && json_type(v) == JSON_STRING && json_str(v, &text))
                sb_add(&text, " ");
            if (json_get(custom, "text", &v) && json_type(v) == JSON_STRING)
                json_str(v, &text);
            while (text.len && text.data[text.len - 1] == ' ')
                text.data[--text.len] = 0;
            if (text.len) {
                m->custom_status = (unsigned)m->strings.len;
                sb_addn(&m->strings, text.data, text.len + 1);
            }
        }
        sb_free(&text);
    }
}


/* ---- READY ---- */

/* Keeps an unavailable server's entry of READY's user_guild_settings ("id\tJSON\n") until it comes back. */
static void add_pending(sb_t *pending, json_t settings, const char *id)
{
    json_t list, e, v;
    json_iter_t it;

    sb_add(pending, id);
    sb_add(pending, "\t");
    if (entries(settings, &list)) {
        json_iter(list, &it);
        while (json_next(&it, NULL, &e))
            if (json_get(e, "guild_id", &v) && id_eq(v, id)) {
                size_t start = pending->len;
                sb_addn(pending, e.p, (size_t)(e.end - e.p));
                for (size_t i = start; i < pending->len; i++)
                    if (pending->data[i] == '\n' || pending->data[i] == '\r')
                        pending->data[i] = ' '; /* whitespace only: raw line breaks are not valid in strings */
                break;
            }
    }
    sb_add(pending, "\n");
}

model_t *model_from_ready(json_t d)
{
    enum { R_USER, R_SETTINGS, R_GUILDS, R_MERGED, R_DMS, R_USERS, R_READS, R_GUILD_SETTINGS, R_COUNT };
    static const char *const keys[R_COUNT] = {"user", "user_settings", "guilds", "merged_members",
                                              "private_channels", "users", "read_state", "user_guild_settings"};
    model_t *m = mem_alloc(sizeof *m);
    json_t r[R_COUNT], user, g, v;
    json_iter_t it;
    unsigned cap = 0, total, i = 0;
    sb_t pending = {0};

    /* READY is megabytes: one pass over its keys, instead of a json_get (a scan) per key. */
    get_keys(d, keys, r, R_COUNT);
    user = r[R_USER];
    sb_addn(&m->strings, "", 1); /* offset 0 is the empty string */
    if (user.p) {
        if (json_get(user, "id", &v))
            json_raw(v, m->user_id, sizeof m->user_id);
        if (json_get(user, "avatar", &v))
            json_raw(v, m->user_avatar, sizeof m->user_avatar);
        if ((json_get(user, "global_name", &v) && json_type(v) == JSON_STRING) || json_get(user, "username", &v))
            m->user_name = add_str(m, v);
        if (json_get(user, "premium_type", &v))
            m->premium = (int)to_i64(v);
    }
    if (json_type(r[R_SETTINGS]) == JSON_OBJECT)
        keep_folder_json(m, r[R_SETTINGS]);

    total = (unsigned)json_count(r[R_GUILDS]);
    m->guilds = mem_alloc((total + 1) * sizeof *m->guilds);
    if (total) {
        json_iter(r[R_GUILDS], &it);
        while (json_next(&it, NULL, &g)) {
            if (field(g, "name", &v)) {
                guild_t *gd = &m->guilds[m->nguilds++];
                build_guild(m, &cap, r[R_MERGED], g, i, 1, gd);
                gd->rank = guild_rank(m, gd->id);
                gd->folder = guild_folder(m, gd->id);
            } else if (json_get(g, "id", &v)) { /* unavailable (an outage): keep its settings for GUILD_CREATE */
                char id[24];
                json_raw(v, id, sizeof id);
                add_pending(&pending, r[R_GUILD_SETTINGS], id);
            }
            i++;
        }
    }
    sort_guilds(m->guilds, m->nguilds);
    if (pending.len) {
        m->pending = (unsigned)m->strings.len;
        sb_addn(&m->strings, pending.data, pending.len + 1);
    }
    sb_free(&pending);
    add_dms(m, &cap, r[R_DMS], r[R_USERS]);
    apply_read_state(m, r[R_READS]);
    apply_mutes(m, r[R_GUILD_SETTINGS]);
    if (json_type(r[R_SETTINGS]) == JSON_OBJECT)
        read_user_settings(m, r[R_SETTINGS]);
    return m;
}

void model_free(model_t *m)
{
    if (!m)
        return;
    sb_free(&m->strings);
    mem_free(m->guilds);
    mem_free(m->channels);
    mem_free(m->hidden);
    mem_free(m->folders);
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
    n->premium = m->premium;
    copy_id(n->status, m->status, sizeof n->status);
    n->custom_status = m->custom_status;
    n->developer_mode = m->developer_mode;
    n->folder_json = m->folder_json;
    n->joined = m->joined;
    n->pending = m->pending;
    n->pending_reads = m->pending_reads;
    n->guilds = mem_alloc((m->nguilds + extra_guilds + 1) * sizeof *n->guilds);
    n->folders = mem_alloc((m->nfolders + 1) * sizeof *n->folders);
    for (unsigned f = 0; f < m->nfolders; f++)
        n->folders[f] = m->folders[f];
    n->nfolders = m->nfolders;
    return n;
}

static void copy_guild(model_t *n, unsigned *cap, const model_t *m, unsigned g)
{
    guild_t *out = &n->guilds[n->nguilds++];

    *out = m->guilds[g];
    out->first = n->nchannels;
    for (unsigned i = m->guilds[g].first; i < m->guilds[g].first + m->guilds[g].count; i++)
        push(n, cap, &m->channels[i]);
    out->hidden_first = n->nhidden;
    for (unsigned i = m->guilds[g].hidden_first; i < m->guilds[g].hidden_first + m->guilds[g].hidden_count; i++)
        push_hidden(n, &m->hidden[i]);
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

/* A copy of m, every server as it was. */
static model_t *clone(const model_t *m)
{
    unsigned cap = 0;
    model_t *n = clone_empty(m, 0);

    for (unsigned g = 0; g < m->nguilds; g++)
        copy_guild(n, &cap, m, g);
    copy_dms(n, &cap, m, NULL, NULL, NULL);
    return n;
}

/*
 * A copy of m where guild gi becomes `edited` (its strings already in n, which
 * clone_empty made) with its channels `all` placed again. Takes `all`.
 */
static model_t *with_guild(model_t *n, const model_t *m, int gi, const guild_t *edited, channel_t *all, unsigned k)
{
    unsigned cap = 0;

    for (unsigned g = 0; g < m->nguilds; g++) {
        if ((int)g != gi) {
            copy_guild(n, &cap, m, g);
            continue;
        }
        n->guilds[n->nguilds] = *edited;
        compute_perms(n, &n->guilds[n->nguilds]);
        place_channels(n, &cap, &n->guilds[n->nguilds], all, k);
        n->nguilds++;
    }
    mem_free(all);
    copy_dms(n, &cap, m, NULL, NULL, NULL);
    return n;
}

/* Guild gi edited by the caller in `g` (strings in n), its channels shown or hidden again. */
static model_t *replace_guild(model_t *n, const model_t *m, int gi, const guild_t *g)
{
    unsigned k;
    channel_t *all = all_channels(m, (unsigned)gi, NULL, 0, &k);

    return with_guild(n, m, gi, g, all, k);
}

/* Keeps what we knew about a channel (read state, mute) across an update: `o` is its old version, if any. */
static void carry_state(channel_t *c, const channel_t *o)
{
    if (!o)
        return;
    copy_id(c->read, o->read, sizeof c->read);
    c->mentions = o->mentions;
    c->muted = o->muted;
    c->mute_until = o->mute_until;
    c->notify = o->notify;
    if (id_cmp(o->last_message, c->last_message) > 0)
        copy_id(c->last_message, o->last_message, sizeof c->last_message);
}

static model_t *apply_channel(const model_t *m, json_t d, int deleted)
{
    json_t v;
    char id[24] = {0}, guild_id[24] = {0};
    int old, gi = -1;
    unsigned cap = 0, k;
    model_t *n;
    channel_t *all;

    if (json_get(d, "id", &v))
        json_raw(v, id, sizeof id);
    if (json_get(d, "guild_id", &v))
        json_raw(v, guild_id, sizeof guild_id);
    old = model_find_channel(m, id);
    if (guild_id[0] && (gi = model_find_guild(m, guild_id)) < 0)
        return NULL;

    if (gi < 0) {
        channel_t c;
        json_t list = {0};
        users_t users;
        if (deleted && old < 0)
            return NULL;
        n = clone_empty(m, 0);
        for (unsigned g = 0; g < m->nguilds; g++)
            copy_guild(n, &cap, m, g);
        if (deleted) {
            copy_dms(n, &cap, m, id, NULL, NULL);
            return n;
        }
        json_get(d, "users", &list);
        users_read(list, &users);
        if (!make_dm(n, &users, d, &c)) {
            model_free(n);
            n = NULL;
        } else {
            carry_state(&c, find_any((model_t *)m, c.id));
            copy_dms(n, &cap, m, old >= 0 ? id : NULL, old >= 0 ? &c : NULL, old >= 0 ? NULL : &c);
        }
        mem_free(users.v);
        return n;
    }

    if (deleted && !find_any((model_t *)m, id))
        return NULL;
    n = clone_empty(m, 0);
    all = all_channels(m, (unsigned)gi, id, 1, &k);
    if (!deleted) {
        all[k] = make_channel(n, d);
        carry_state(&all[k], find_any((model_t *)m, all[k].id));
        k++;
    }
    return with_guild(n, m, gi, &m->guilds[gi], all, k);
}

/* Copies the word at p (up to a space or a line end) and returns what follows it and its space. */
static const char *word(const char *p, char *out, size_t size)
{
    size_t k = 0;

    while (*p && *p != ' ' && *p != '\n') {
        if (k + 1 < size)
            out[k++] = *p;
        p++;
    }
    out[k] = 0;
    return p + (*p == ' ');
}

/* A pending server's line ("id\t...") is in the list. */
static int pending_has(const char *p, const char *id)
{
    size_t n = sc_strlen(id);

    while (*p) {
        if (str_eq_n(p, id, n) && p[n] == '\t')
            return 1;
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    return 0;
}

/* The settings and read states kept at READY for a server that was unavailable; drops them from n. */
static void apply_pending(model_t *n, const char *id)
{
    const char *p = n->pending ? n->strings.data + n->pending : "";
    sb_t rest = {0}, entry = {0}, reads = {0};
    size_t idn = sc_strlen(id);
    json_t e;
    chan_index_t ix;

    while (*p) {
        const char *tab = p, *end;
        while (*tab && *tab != '\t' && *tab != '\n')
            tab++;
        for (end = tab; *end && *end != '\n'; end++)
            ;
        if ((size_t)(tab - p) == idn && str_eq_n(p, id, idn) && *tab == '\t')
            sb_addn(&entry, tab + 1, (size_t)(end - tab - 1));
        else
            sb_addn(&rest, p, (size_t)(end - p + (*end == '\n')));
        p = end + (*end == '\n');
    }
    if (n->pending_reads)
        sb_add(&reads, n->strings.data + n->pending_reads);
    n->pending = 0;
    if (rest.len) {
        n->pending = (unsigned)n->strings.len;
        sb_addn(&n->strings, rest.data, rest.len + 1);
    }
    index_build(&ix, n);
    if (entry.len && json_parse(entry.data, entry.len, &e))
        apply_settings_entry(&ix, e);
    /* "channel last_message mentions" lines; last_message "-" when there was none. */
    for (const char *r = reads.data ? reads.data : ""; *r;) {
        char cid[24], last[24];
        unsigned long long count;
        channel_t *c;
        r = word(r, cid, sizeof cid);
        r = word(r, last, sizeof last);
        count = read_u64(&r);
        while (*r && *r != '\n')
            r++;
        if (*r)
            r++;
        if ((c = index_find(&ix, cid)) != NULL) {
            if (last[0] != '-')
                copy_id(c->read, last, sizeof c->read);
            c->mentions = (int)count;
        }
    }
    mem_free(ix.slot);
    sb_free(&rest);
    sb_free(&entry);
    sb_free(&reads);
}

static model_t *apply_guild_create(const model_t *m, json_t d)
{
    json_t v, none = {0};
    char id[24] = {0};
    int gi, pending;
    unsigned cap = 0, placed = 0;
    guild_t fresh;
    model_t *n;

    if (json_get(d, "id", &v))
        json_raw(v, id, sizeof id);
    if (!field(d, "name", &v))
        return NULL; /* unavailable */
    gi = model_find_guild(m, id);
    pending = gi < 0 && m->pending && pending_has(m->strings.data + m->pending, id);
    n = clone_empty(m, 1);
    /* Built apart, then its channels are moved into place with the others. */
    {
        model_t *tmp = clone_empty(m, 0);
        unsigned tcap = 0;
        channel_t *all;
        unsigned k;
        chan_index_t ix;
        build_guild(tmp, &tcap, none, d, 0, 0, &fresh);
        /* The strings it added go to n at the same offsets: n is a copy of m's strings too. */
        sb_addn(&n->strings, tmp->strings.data + m->strings.len, tmp->strings.len - m->strings.len);
        n->joined = tmp->joined;
        k = fresh.count + fresh.hidden_count;
        all = mem_alloc((k + 1) * sizeof *all);
        for (unsigned i = 0; i < fresh.count; i++)
            all[i] = tmp->channels[fresh.first + i];
        for (unsigned i = 0; i < fresh.hidden_count; i++)
            all[fresh.count + i] = tmp->hidden[fresh.hidden_first + i];
        index_build(&ix, (model_t *)m);
        for (unsigned i = 0; i < k; i++)
            carry_state(&all[i], index_find(&ix, all[i].id));
        mem_free(ix.slot);
        model_free(tmp);
        if (gi >= 0) {
            fresh.muted = m->guilds[gi].muted;
            fresh.mute_until = m->guilds[gi].mute_until;
            fresh.notify = m->guilds[gi].notify;
            fresh.suppress_everyone = m->guilds[gi].suppress_everyone;
            fresh.suppress_roles = m->guilds[gi].suppress_roles;
            fresh.folder = m->guilds[gi].folder;
            fresh.rank = m->guilds[gi].rank;
        } else {
            /* Back from an outage, or joined: its place in the user's order, else on top like Discord. */
            fresh.rank = guild_rank(n, fresh.id);
            fresh.folder = guild_folder(n, fresh.id);
        }
        for (unsigned g = 0; g <= m->nguilds; g++) {
            int here = gi >= 0 ? (int)g == gi
                               : !placed && (fresh.rank < 0 || g == m->nguilds || m->guilds[g].rank > fresh.rank);
            if (here) {
                n->guilds[n->nguilds] = fresh;
                place_channels(n, &cap, &n->guilds[n->nguilds], all, k);
                n->nguilds++;
                placed = 1;
                if (gi >= 0)
                    continue;
            }
            if (g < m->nguilds && (int)g != gi)
                copy_guild(n, &cap, m, g);
        }
        mem_free(all);
    }
    copy_dms(n, &cap, m, NULL, NULL, NULL);
    if (pending)
        apply_pending(n, id);
    return n;
}

static model_t *apply_guild_patch(const model_t *m, const char *event, json_t d)
{
    json_t v, user;
    char id[24] = {0};
    int gi;
    unsigned cap = 0;
    model_t *n;
    guild_t g;

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
    if (str_eq(event, "GUILD_DELETE")) {
        for (unsigned k = 0; k < m->nguilds; k++)
            if ((int)k != gi)
                copy_guild(n, &cap, m, k);
        copy_dms(n, &cap, m, NULL, NULL, NULL);
        return n;
    }
    g = m->guilds[gi];
    if (str_eq(event, "GUILD_MEMBER_UPDATE")) {
        /* Our roles changed: some channels may show or hide. */
        role_id_t mine[MAX_ROLES];
        json_t roles;
        int nmine = json_get(d, "roles", &roles) ? read_roles(roles, mine) : 0;
        g.my_roles = roles_string(n, mine, nmine);
        g.roles_known = 1;
    } else {
        /* GUILD_UPDATE: name, icon, owner, default notifications, and the roles if they came along. */
        if (field(d, "name", &v))
            g.name = add_str(n, v);
        if (field(d, "icon", &v))
            json_raw(v, g.icon, sizeof g.icon);
        if (field(d, "owner_id", &v))
            g.owner = id_eq(v, m->user_id);
        if (field(d, "default_message_notifications", &v))
            g.default_notify = to_i64(v) == 1 ? NOTIFY_MENTIONS : NOTIFY_ALL;
        if (field(d, "roles", &v))
            g.roles = pack_roles(n, v);
    }
    return replace_guild(n, m, gi, &g);
}

/* GUILD_ROLE_CREATE / UPDATE carry {guild_id, role}, GUILD_ROLE_DELETE {guild_id, role_id}. */
static model_t *apply_role(const model_t *m, json_t d, int deleted)
{
    json_t v, role = {0};
    char gid[24] = "", rid[24] = "";
    unsigned cursor = 0;
    model_role_t r;
    model_t *n;
    sb_t packed = {0};
    guild_t g;
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
    g = m->guilds[gi];
    g.roles = (unsigned)n->strings.len;
    if (packed.len)
        sb_addn(&n->strings, packed.data, packed.len);
    sb_addn(&n->strings, "", 1);
    sb_free(&packed);
    /* A role's permissions (ours or @everyone's) decide what we see. */
    return replace_guild(n, m, gi, &g);
}

/* GUILD_EMOJIS_UPDATE or GUILD_STICKERS_UPDATE: the whole list is replaced. */
static model_t *apply_emojis(const model_t *m, json_t d, int stickers)
{
    json_t v, list;
    char gid[24] = "";
    int gi;
    model_t *n;

    if (json_get(d, "guild_id", &v))
        json_raw(v, gid, sizeof gid);
    if ((gi = model_find_guild(m, gid)) < 0 || !json_get(d, stickers ? "stickers" : "emojis", &list))
        return NULL;
    n = clone(m);
    if (stickers)
        n->guilds[gi].stickers = pack_emojis(n, list, 1);
    else
        n->guilds[gi].emojis = pack_emojis(n, list, 0);
    return n;
}

/* USER_GUILD_SETTINGS_UPDATE: one server's settings (or the DMs') changed, here or elsewhere. */
static model_t *apply_settings(const model_t *m, json_t d)
{
    model_t *n = clone(m);
    chan_index_t ix;

    index_build(&ix, n);
    apply_settings_entry(&ix, d);
    mem_free(ix.slot);
    return n;
}

/* USER_SETTINGS_UPDATE; new server folders or order rebuild the list, in the new order. */
static model_t *apply_user_settings(const model_t *m, json_t d)
{
    json_t v;
    model_t *n = clone_empty(m, 0);
    unsigned cap = 0, *order;
    guild_t *sorted;

    read_user_settings(n, d);
    if (!json_get(d, "guild_folders", &v) && !json_get(d, "guild_positions", &v)) {
        for (unsigned g = 0; g < m->nguilds; g++)
            copy_guild(n, &cap, m, g);
        copy_dms(n, &cap, m, NULL, NULL, NULL);
        return n;
    }
    keep_folder_json(n, d);
    n->nfolders = 0;
    /* Ranks and folders again, then the guilds (with their channels) in that order. */
    sorted = mem_alloc((m->nguilds + 1) * sizeof *sorted);
    order = mem_alloc((m->nguilds + 1) * sizeof *order);
    for (unsigned g = 0; g < m->nguilds; g++) {
        sorted[g] = m->guilds[g];
        sorted[g].rank = guild_rank(n, m->guilds[g].id);
        sorted[g].folder = -1;
        sorted[g].first = g; /* the source index, while sorting */
    }
    sort_guilds(sorted, m->nguilds);
    for (unsigned g = 0; g < m->nguilds; g++)
        order[g] = sorted[g].first;
    for (unsigned k = 0; k < m->nguilds; k++) {
        unsigned g = order[k];
        copy_guild(n, &cap, m, g);
        n->guilds[k].rank = sorted[k].rank;
        n->guilds[k].folder = guild_folder(n, n->guilds[k].id);
    }
    copy_dms(n, &cap, m, NULL, NULL, NULL);
    mem_free(order);
    mem_free(sorted);
    return n;
}

/* Threads we joined or left, here or on another device. */
static model_t *apply_thread_members(const model_t *m, const char *event, json_t d)
{
    json_t v, list;
    json_iter_t it;
    char id[24] = "";
    model_t *n;

    if (json_get(d, "id", &v))
        json_raw(v, id, sizeof id);
    if (!id[0])
        return NULL;
    if (str_eq(event, "THREAD_MEMBER_UPDATE")) {
        if (json_get(d, "user_id", &v) && !id_eq(v, m->user_id))
            return NULL;
        if (joined_has(m, id))
            return NULL;
        n = clone(m);
        joined_set(n, id, 1);
        return n;
    }
    /* THREAD_MEMBERS_UPDATE: we may be among the removed. */
    if (!json_get(d, "removed_member_ids", &list))
        return NULL;
    json_iter(list, &it);
    while (json_next(&it, NULL, &v))
        if (id_eq(v, m->user_id)) {
            json_t gid;
            n = json_get(d, "guild_id", &gid) ? apply_channel(m, d, 1) : NULL;
            if (!n)
                n = clone(m);
            joined_set(n, id, 0);
            return n;
        }
    return NULL;
}

unsigned long long model_permissions(const model_t *m, unsigned i)
{
    int gi = model_channel_guild(m, i), p;
    const channel_t *c = &m->channels[i];
    const guild_t *g;
    role_id_t mine[MAX_ROLES];

    if (gi < 0)
        return ~0ull;
    g = &m->guilds[gi];
    if (g->sees_all)
        return ~0ull; /* owner, administrator, or roles not known yet */
    if (model_is_thread(c->type) && c->parent[0] && (p = model_find_channel(m, c->parent)) >= 0)
        c = &m->channels[p];
    return channel_perms(m, c, g, mine, parse_roles(m, g->my_roles, mine));
}

model_t *model_apply(const model_t *m, const char *event, json_t d)
{
    if (str_eq(event, "THREAD_CREATE") || str_eq(event, "THREAD_UPDATE")) {
        json_t v, member, owner, meta, arch;
        char id[24] = "";
        int known = json_get(d, "id", &v) ? (json_raw(v, id, sizeof id), find_any((model_t *)m, id) != NULL) : 0;
        int ours = json_get(d, "member", &member) || (json_get(d, "owner_id", &owner) && id_eq(owner, m->user_id)) ||
                   joined_has(m, id);
        model_t *n;
        if (json_get(d, "thread_metadata", &meta) && json_get(meta, "archived", &arch) && is_true(arch))
            return known ? apply_channel(m, d, 1) : NULL; /* archived: gone from the list, still joined */
        if (!known && !ours)
            return NULL;
        n = apply_channel(m, d, 0);
        if (n && ours)
            joined_set(n, id, 1);
        return n;
    }
    if (str_eq(event, "THREAD_DELETE"))
        return apply_channel(m, d, 1);
    if (str_eq(event, "THREAD_MEMBER_UPDATE") || str_eq(event, "THREAD_MEMBERS_UPDATE"))
        return apply_thread_members(m, event, d);
    if (str_eq(event, "USER_SETTINGS_UPDATE"))
        return apply_user_settings(m, d);
    if (str_eq(event, "USER_GUILD_SETTINGS_UPDATE"))
        return apply_settings(m, d);
    if (str_eq(event, "GUILD_EMOJIS_UPDATE"))
        return apply_emojis(m, d, 0);
    if (str_eq(event, "GUILD_STICKERS_UPDATE"))
        return apply_emojis(m, d, 1);
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
