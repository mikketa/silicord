#include <windows.h>
#include "memberlist.h"
#include "mem.h"

static void item_free(ml_item_t *it)
{
    sb_free(&it->name);
    sb_free(&it->roles);
    sb_free(&it->activity);
    *it = (ml_item_t){0};
}

static void reserve(ml_t *l, int n)
{
    if (n <= l->cap)
        return;
    while (l->cap < n)
        l->cap = l->cap ? l->cap * 2 : 128;
    l->items = mem_realloc(l->items, (size_t)l->cap * sizeof *l->items);
}

/* Grows the list with empty slots up to n items. */
static void grow(ml_t *l, int n)
{
    reserve(l, n);
    while (l->n < n)
        l->items[l->n++] = (ml_item_t){0};
}

int ml_status(json_t s)
{
    if (json_str_eq(s, "online"))
        return ML_ONLINE;
    if (json_str_eq(s, "idle"))
        return ML_IDLE;
    if (json_str_eq(s, "dnd"))
        return ML_DND;
    if (json_str_eq(s, "offline") || json_str_eq(s, "invisible"))
        return ML_OFFLINE;
    return ML_UNKNOWN;
}

static int get_raw(json_t obj, const char *key, char *dst, size_t size)
{
    json_t v;

    if (!json_get(obj, key, &v) || json_type(v) != JSON_STRING)
        return 0;
    json_raw(v, dst, size);
    return 1;
}

static int get_str(json_t obj, const char *key, sb_t *out)
{
    json_t v;

    return json_get(obj, key, &v) && json_type(v) == JSON_STRING && v.end - v.p > 2 && json_str(v, out);
}

/* Text shown under a member's name: custom status, else the first activity. */
void ml_activity(json_t presence, sb_t *out)
{
    json_t acts, a, v;
    json_iter_t it;
    long long type;

    if (!json_get(presence, "activities", &acts))
        return;
    json_iter(acts, &it);
    while (json_next(&it, NULL, &a)) {
        if (!json_get(a, "type", &v) || !json_int(v, &type))
            continue;
        if (type == 4) { /* custom status, shown instead of any game */
            sb_t state = {0};
            if (get_str(a, "state", &state)) {
                sb_clear(out);
                sb_addn(out, state.data, state.len);
                sb_free(&state);
                return;
            }
            continue;
        }
        if (out->len)
            continue;
        sb_add(out, type == 0 ? "Playing " : type == 1 ? "Streaming " : type == 2 ? "Listening to " :
                    type == 3 ? "Watching " : type == 5 ? "Competing in " : "");
        if (!get_str(a, "name", out))
            sb_clear(out);
    }
}

static void parse_item(json_t obj, ml_item_t *out)
{
    json_t g, m, user, v, roles, role, presence;
    json_iter_t it;
    char id[24];

    item_free(out);
    out->valid = 1;
    if (json_get(obj, "group", &g)) {
        long long count = 0;
        out->group = 1;
        get_raw(g, "id", out->id, sizeof out->id);
        if (json_get(g, "count", &v))
            json_int(v, &count);
        out->count = (int)count;
        return;
    }
    if (!json_get(obj, "member", &m) || !json_get(m, "user", &user)) {
        out->valid = 0;
        return;
    }
    get_raw(user, "id", out->id, sizeof out->id);
    if (!get_raw(m, "avatar", out->avatar, sizeof out->avatar))
        get_raw(user, "avatar", out->avatar, sizeof out->avatar);
    if (!get_str(m, "nick", &out->name) && !get_str(user, "global_name", &out->name))
        get_str(user, "username", &out->name);
    out->bot = json_get(user, "bot", &v) && json_type(v) == JSON_TRUE;
    if (json_get(m, "roles", &roles)) {
        json_iter(roles, &it);
        while (json_next(&it, NULL, &role)) {
            json_raw(role, id, sizeof id);
            if (out->roles.len)
                sb_add(&out->roles, ",");
            sb_add(&out->roles, id);
        }
    }
    if (json_get(m, "presence", &presence)) {
        out->status = json_get(presence, "status", &v) ? ml_status(v) : ML_UNKNOWN;
        ml_activity(presence, &out->activity);
    } else {
        out->status = ML_OFFLINE;
    }
}

static int range_of(json_t op, int *a, int *b)
{
    json_t r, v;
    json_iter_t it;
    long long n[2] = {0, -1};
    int k = 0;

    if (!json_get(op, "range", &r))
        return 0;
    json_iter(r, &it);
    while (k < 2 && json_next(&it, NULL, &v))
        json_int(v, &n[k++]);
    *a = (int)n[0];
    *b = (int)n[1];
    return k == 2 && *a >= 0 && *b >= *a && *b < 1000000;
}

int ml_apply(ml_t *l, json_t d)
{
    json_t v, ops, op, items, item;
    json_iter_t it, iit;
    char guild[24] = "", id[32] = "";
    long long count;
    int changed = 0;

    get_raw(d, "guild_id", guild, sizeof guild);
    get_raw(d, "id", id, sizeof id);
    if (lstrcmpA(guild, l->guild) != 0 || lstrcmpA(id, l->list_id) != 0) {
        ml_free(l);
        lstrcpynA(l->guild, guild, sizeof l->guild);
        lstrcpynA(l->list_id, id, sizeof l->list_id);
        changed = 1;
    }
    if (json_get(d, "member_count", &v) && json_int(v, &count))
        l->member_count = (int)count;
    if (json_get(d, "online_count", &v) && json_int(v, &count))
        l->online_count = (int)count;
    if (json_get(d, "groups", &v)) {
        json_t g, c;
        l->ngroups = 0;
        json_iter(v, &it);
        while (l->ngroups < 64 && json_next(&it, NULL, &g)) {
            long long n = 0;
            get_raw(g, "id", l->groups[l->ngroups].id, sizeof l->groups[0].id);
            if (json_get(g, "count", &c))
                json_int(c, &n);
            l->groups[l->ngroups++].count = (int)n;
        }
        changed = 1;
    }
    if (!json_get(d, "ops", &ops))
        return changed;
    json_iter(ops, &it);
    while (json_next(&it, NULL, &op)) {
        json_t kind;
        long long index = -1;
        int a, b;

        if (!json_get(op, "op", &kind))
            continue;
        if (json_get(op, "index", &v))
            json_int(v, &index);
        if (json_str_eq(kind, "SYNC") && range_of(op, &a, &b) && json_get(op, "items", &items)) {
            int k = a;
            grow(l, b + 1);
            json_iter(items, &iit);
            while (k <= b && json_next(&iit, NULL, &item))
                parse_item(item, &l->items[k++]);
            changed = 1;
        } else if (json_str_eq(kind, "INVALIDATE") && range_of(op, &a, &b)) {
            for (int k = a; k <= b && k < l->n; k++)
                item_free(&l->items[k]);
            changed = 1;
        } else if (json_str_eq(kind, "INSERT") && index >= 0 && index <= l->n && index < 1000000 &&
                   json_get(op, "item", &item)) {
            reserve(l, l->n + 1);
            for (int k = l->n; k > (int)index; k--)
                l->items[k] = l->items[k - 1];
            l->items[index] = (ml_item_t){0};
            l->n++;
            parse_item(item, &l->items[index]);
            changed = 1;
        } else if (json_str_eq(kind, "UPDATE") && index >= 0 && index < 1000000 && json_get(op, "item", &item)) {
            grow(l, (int)index + 1);
            parse_item(item, &l->items[index]);
            changed = 1;
        } else if (json_str_eq(kind, "DELETE") && index >= 0 && index < l->n) {
            item_free(&l->items[index]);
            for (int k = (int)index; k < l->n - 1; k++)
                l->items[k] = l->items[k + 1];
            l->n--;
            changed = 1;
        }
    }
    return changed;
}

int ml_group_count(const ml_t *l, const char *id)
{
    for (int i = 0; i < l->ngroups; i++)
        if (lstrcmpA(l->groups[i].id, id) == 0)
            return l->groups[i].count;
    return 0;
}

void ml_free(ml_t *l)
{
    for (int i = 0; i < l->n; i++)
        item_free(&l->items[i]);
    mem_free(l->items);
    *l = (ml_t){0};
}
