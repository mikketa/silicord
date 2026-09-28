#include <windows.h>
#include "command.h"

#define MAX_OPTS 25 /* Discord's limit per command */

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* The NUL-terminated `a` equals the n bytes at s, ignoring ASCII case. */
static int same_ci(const char *a, const char *s, size_t n)
{
    size_t k;

    for (k = 0; k < n && a[k]; k++)
        if (lower((unsigned char)a[k]) != lower((unsigned char)s[k]))
            return 0;
    return k == n && !a[k];
}

/* A string value equals the n bytes at s, ignoring ASCII case. */
static int name_is(json_t v, const char *s, size_t n)
{
    char buf[112];

    if (json_type(v) != JSON_STRING)
        return 0;
    json_raw(v, buf, sizeof buf);
    return same_ci(buf, s, n);
}

static int get_type(json_t obj)
{
    json_t v;
    long long t = 1;

    if (json_get(obj, "type", &v))
        json_int(v, &t);
    return (int)t;
}

static int is_true_field(json_t obj, const char *key)
{
    json_t v;

    return json_get(obj, key, &v) && json_type(v) == JSON_TRUE;
}

int cmd_next(json_t index, json_iter_t *it, json_t *cmd)
{
    json_t list;

    if (!it->p) {
        if (!json_get(index, "application_commands", &list) || json_type(list) != JSON_ARRAY)
            return 0;
        json_iter(list, it);
    }
    while (json_next(it, NULL, cmd))
        if (get_type(*cmd) == 1) /* chat input; 2 and 3 are context menu commands */
            return 1;
    return 0;
}

int cmd_find(json_t index, const char *name, size_t n, json_t *out)
{
    json_iter_t it = {0};
    json_t v;

    while (cmd_next(index, &it, out))
        if (json_get(*out, "name", &v) && name_is(v, name, n))
            return 1;
    return 0;
}

int cmd_app(json_t index, const char *app_id, json_t *out)
{
    json_t list, v;
    json_iter_t it;

    if (!json_get(index, "applications", &list))
        return 0;
    json_iter(list, &it);
    while (json_next(&it, NULL, out))
        if (json_get(*out, "id", &v) && json_str_eq(v, app_id))
            return 1;
    return 0;
}

/* The next word of s[*i..n), skipping spaces; returns its length. */
static size_t next_word(const char *s, size_t n, size_t *i)
{
    size_t start;

    while (*i < n && s[*i] == ' ')
        (*i)++;
    start = *i;
    while (*i < n && s[*i] != ' ')
        (*i)++;
    return *i - start;
}

static int has_subcommands(json_t opts)
{
    json_iter_t it;
    json_t o;

    if (!opts.p)
        return 0;
    json_iter(opts, &it);
    while (json_next(&it, NULL, &o))
        if (get_type(o) == OPT_SUB_COMMAND || get_type(o) == OPT_SUB_GROUP)
            return 1;
    return 0;
}

/* Same as cmd_leaf, also giving the subcommands walked (at most two). */
static int walk(json_t cmd, const char *args, size_t n, json_t *opts, size_t *used, json_t *path, int *depth)
{
    json_t list = {0};

    *depth = 0;
    *used = 0;
    if (!json_get(cmd, "options", &list) || json_type(list) != JSON_ARRAY)
        list = (json_t){0};
    while (has_subcommands(list) && *depth < 2) {
        size_t i = *used, len = next_word(args, n, &i);
        json_iter_t it;
        json_t o, v, found = {0};
        json_iter(list, &it);
        while (len && json_next(&it, NULL, &o))
            if ((get_type(o) == OPT_SUB_COMMAND || get_type(o) == OPT_SUB_GROUP) && json_get(o, "name", &v) &&
                name_is(v, args + i - len, len))
                found = o;
        if (!found.p) {
            *opts = list;
            return 0;
        }
        path[(*depth)++] = found;
        *used = i;
        if (!json_get(found, "options", &list) || json_type(list) != JSON_ARRAY)
            list = (json_t){0};
    }
    *opts = list;
    return 1;
}

int cmd_leaf(json_t cmd, const char *args, size_t n, json_t *opts, size_t *used)
{
    json_t path[2];
    int depth;

    return walk(cmd, args, n, opts, used, path, &depth);
}

static void trim(const char *s, size_t *a, size_t *b)
{
    while (*a < *b && s[*a] == ' ')
        (*a)++;
    while (*b > *a && s[*b - 1] == ' ')
        (*b)--;
}

static void fail(char *err, size_t errn, const char *what, json_t opt)
{
    char name[40] = "";
    json_t v;

    if (json_get(opt, "name", &v))
        json_raw(v, name, sizeof name);
    if (errn > 96)
        wsprintfA(err, what, name);
}

/* Appends the JSON value of option `opt` typed as s[0..n); 0 if it does not fit. */
static int add_value(sb_t *out, json_t opt, const char *s, size_t n)
{
    int type = get_type(opt);
    json_t choices, c, v;
    json_iter_t it;
    size_t i = 0;

    if (json_get(opt, "choices", &choices) && json_type(choices) == JSON_ARRAY && json_count(choices)) {
        json_iter(choices, &it);
        while (json_next(&it, NULL, &c)) {
            int match = json_get(c, "name", &v) && name_is(v, s, n);
            if (!match && json_get(c, "value", &v)) /* the value itself, a string or a number */
                match = json_type(v) == JSON_STRING ? name_is(v, s, n)
                                                    : (size_t)(v.end - v.p) == n && same_ci(v.p, s, n);
            if (match) {
                json_get(c, "value", &v);
                sb_addn(out, v.p, (size_t)(v.end - v.p));
                return 1;
            }
        }
        return 0;
    }
    switch (type) {
    case OPT_STRING:
        sb_json_str(out, s, n);
        return 1;
    case OPT_INTEGER:
    case OPT_NUMBER: {
        int digits = 0, dot = 0;
        if (i < n && s[i] == '-')
            i++;
        for (; i < n; i++) {
            if (s[i] >= '0' && s[i] <= '9')
                digits++;
            else if (s[i] == '.' && type == OPT_NUMBER && !dot)
                dot = 1;
            else
                return 0;
        }
        if (!digits)
            return 0;
        sb_addn(out, s, n);
        return 1;
    }
    case OPT_BOOLEAN: {
        static const char *const yes[] = {"true", "yes", "on", "1"}, *const no[] = {"false", "no", "off", "0"};
        for (int k = 0; k < 4; k++) {
            if (same_ci(yes[k], s, n)) {
                sb_add(out, "true");
                return 1;
            }
            if (same_ci(no[k], s, n)) {
                sb_add(out, "false");
                return 1;
            }
        }
        return 0;
    }
    case OPT_USER:
    case OPT_CHANNEL:
    case OPT_ROLE:
    case OPT_MENTIONABLE: {
        /* <@id>, <@!id>, <#id>, <@&id> or the id itself. */
        size_t a, b;
        while (i < n && (s[i] < '0' || s[i] > '9'))
            i++;
        a = i;
        while (i < n && s[i] >= '0' && s[i] <= '9')
            i++;
        b = i;
        if (b - a < 5 || b - a > 20)
            return 0;
        sb_add(out, "\"");
        sb_addn(out, s + a, b - a);
        sb_add(out, "\"");
        return 1;
    }
    default: /* attachments need an upload first */
        return 0;
    }
}

/* The options of the leaf, as a JSON array, from s[0..n). */
static int build_leaf(json_t list, const char *s, size_t n, sb_t *out, char *err, size_t errn)
{
    json_t opts[MAX_OPTS], o, v;
    size_t from[MAX_OPTS], to[MAX_OPTS], mark[MAX_OPTS];
    int count = 0, given[MAX_OPTS] = {0}, first = 1;
    size_t lead_end = n;
    json_iter_t it;

    if (list.p) {
        json_iter(list, &it);
        while (count < MAX_OPTS && json_next(&it, NULL, &o))
            opts[count++] = o;
    }
    /* "name:" at the start of a word begins that option's value. */
    for (size_t i = 0; i < n; i++) {
        if (i && s[i - 1] != ' ')
            continue;
        for (int k = 0; k < count; k++) {
            char name[40];
            size_t len;
            if (given[k] || !json_get(opts[k], "name", &v))
                continue;
            json_raw(v, name, sizeof name);
            len = (size_t)lstrlenA(name);
            if (len && i + len < n && s[i + len] == ':' && same_ci(name, s + i, len)) {
                given[k] = 1;
                mark[k] = i;
                from[k] = i + len + 1;
                if (i < lead_end)
                    lead_end = i;
                break;
            }
        }
    }
    for (int k = 0; k < count; k++) {
        if (!given[k])
            continue;
        to[k] = n;
        for (int j = 0; j < count; j++)
            if (given[j] && mark[j] > mark[k] && mark[j] < to[k])
                to[k] = mark[j];
        trim(s, &from[k], &to[k]);
    }
    /* Words before any "name:" fill the first option left, like typing a lone value. */
    {
        size_t a = 0, b = lead_end;
        trim(s, &a, &b);
        if (b > a) {
            int k = 0;
            while (k < count && given[k])
                k++;
            if (k == count) {
                if (errn > 64)
                    lstrcpynA(err, "This command takes no more values", (int)errn);
                return 0;
            }
            given[k] = 1;
            from[k] = a;
            to[k] = b;
        }
    }
    sb_add(out, "[");
    for (int k = 0; k < count; k++) {
        char name[40];
        if (!given[k] || to[k] == from[k]) {
            if (is_true_field(opts[k], "required")) {
                fail(err, errn, "Missing required option \"%s\"", opts[k]);
                return 0;
            }
            continue;
        }
        json_get(opts[k], "name", &v);
        json_raw(v, name, sizeof name);
        if (!first)
            sb_add(out, ",");
        first = 0;
        sb_add(out, "{\"type\":");
        sb_i64(out, get_type(opts[k]));
        sb_add(out, ",\"name\":\"");
        sb_add(out, name);
        sb_add(out, "\",\"value\":");
        if (!add_value(out, opts[k], s + from[k], to[k] - from[k])) {
            fail(err, errn, get_type(opts[k]) == OPT_ATTACHMENT ? "Attachments aren't supported (\"%s\")"
                                                                 : "Invalid value for \"%s\"",
                 opts[k]);
            return 0;
        }
        sb_add(out, "}");
    }
    sb_add(out, "]");
    return 1;
}

int cmd_build(json_t cmd, const char *args, size_t n, sb_t *out, char *err, size_t errn)
{
    json_t leaf, path[2], v;
    size_t used;
    int depth;
    sb_t opts = {0};
    char buf[40];

    if (errn)
        err[0] = 0;
    if (!walk(cmd, args, n, &leaf, &used, path, &depth)) {
        if (errn > 64)
            lstrcpynA(err, "Pick a subcommand", (int)errn);
        return 0;
    }
    if (!build_leaf(leaf, args + used, n - used, &opts, err, errn)) {
        sb_free(&opts);
        return 0;
    }
    sb_add(out, "{\"version\":\"");
    if (json_get(cmd, "version", &v)) {
        json_raw(v, buf, sizeof buf);
        sb_add(out, buf);
    }
    sb_add(out, "\",\"id\":\"");
    if (json_get(cmd, "id", &v)) {
        json_raw(v, buf, sizeof buf);
        sb_add(out, buf);
    }
    sb_add(out, "\",\"name\":\"");
    if (json_get(cmd, "name", &v)) {
        json_raw(v, buf, sizeof buf);
        sb_add(out, buf);
    }
    sb_add(out, "\",\"type\":1,\"options\":");
    /* Subcommands wrap the leaf's options: [{type, name, options: [...]}]. */
    for (int d = 0; d < depth; d++) {
        json_get(path[d], "name", &v);
        json_raw(v, buf, sizeof buf);
        sb_add(out, "[{\"type\":");
        sb_i64(out, get_type(path[d]));
        sb_add(out, ",\"name\":\"");
        sb_add(out, buf);
        sb_add(out, "\",\"options\":");
    }
    sb_addn(out, opts.data, opts.len);
    for (int d = 0; d < depth; d++)
        sb_add(out, "}]");
    sb_add(out, ",\"application_command\":");
    sb_addn(out, cmd.p, (size_t)(cmd.end - cmd.p));
    sb_add(out, ",\"attachments\":[]}");
    sb_free(&opts);
    return 1;
}
