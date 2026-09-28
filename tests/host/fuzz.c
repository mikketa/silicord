/*
 * Fuzzer for the portable core, run under AddressSanitizer and UBSan on Linux.
 *
 *   fuzz [iterations] [all|json|text|command|inflate|apng] [seed]
 *
 * Deterministic for a given seed. JSON consumers (messages, READY and gateway
 * events, profiles, member lists, slash commands) get documents that are
 * generated from the keys the code looks up, or made by replacing values in
 * the JSON of the unit tests. The inflater is checked against zlib. Any
 * sanitizer report or broken invariant aborts with a message.
 */
#include <windows.h>
#include <zlib.h>
#include "apng.h"
#include "b64.h"
#include "command.h"
#include "emoji.h"
#include "inflate.h"
#include "json.h"
#include "md.h"
#include "mem.h"
#include "memberlist.h"
#include "model.h"
#include "msg.h"
#include "profile.h"
#include "qr.h"
#include "search.h"

static const char *const k_keys[] = {
#include "keys.inc"
};
static const char *const k_strs[] = {
#include "strs.inc"
    "online", "idle", "dnd", "invisible", "offline", "everyone", "SYNC", "INSERT", "UPDATE", "DELETE", "INVALIDATE",
};
static const struct {
    const char *s;
    size_t n;
} k_seeds[] = {
#include "seeds.inc"
};
static const unsigned char k_apng[] = {
#include "apng.inc"
};

#define COUNT(a) (sizeof(a) / sizeof *(a))

static unsigned long long g_rng = 88172645463325252ull;

static unsigned rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (unsigned)(g_rng >> 11);
}

static unsigned rn(size_t n)
{
    return n ? rnd() % (unsigned)n : 0;
}

static void fail(const char *what, const char *input)
{
    fprintf(stderr, "fuzz: %s\ninput: %s\n", what, input ? input : "");
    abort();
}

/* ---- Generated JSON ---- */

static const char *const k_pieces[] = {
    "\\u00e9", "\\ud83d\\ude00", "\\ud83d", "\\n", "\\\"", "\\\\", "<@123>", "<#456>", "<@&789>", "<:x:1>", "<a:y:2>",
    "**", "__", "*", "_", "~~", "||", "`", "```", "> ", ">>> ", "# ", "## ", "-# ", "- ", "1. ", "[a](https://x.y)",
    "https://a.b/c", "<t:1700000000:R>", "\xee\x80\x80", "\xee\x80\x81", "\xee\x80\x82", "\xee\x80\x83", "\xee\x80\x84",
    "\xf0\x9f\x98\x80", "\xc3", "\xe2\x80", "\xc3\xa9", "\\u0000", " ", ":", "a", "\\t",
};

static void gen_string(sb_t *o)
{
    sb_addn(o, "\"", 1);
    switch (rn(6)) {
    case 0:
        sb_add(o, k_strs[rn(COUNT(k_strs))]);
        break;
    case 1: /* snowflake-like */
        for (unsigned i = 0, n = 1 + rn(20); i < n; i++) {
            char c = (char)('0' + rn(10));
            sb_addn(o, &c, 1);
        }
        break;
    case 2:
        for (unsigned i = 0, n = rn(40); i < n; i++)
            sb_add(o, k_pieces[rn(COUNT(k_pieces))]);
        break;
    case 3:
        for (unsigned i = 0, n = rn(300); i < n; i++) {
            char c = (char)(' ' + rn(95));
            sb_addn(o, c == '"' || c == '\\' ? "x" : &c, 1);
        }
        break;
    case 4:
        sb_add(o, rn(2) ? "2026-09-28T12:34:56.789+00:00" : "2026-02-30T25:61:00");
        break;
    default:
        sb_add(o, k_keys[rn(COUNT(k_keys))]);
        break;
    }
    sb_addn(o, "\"", 1);
}

static void gen_value(sb_t *o, int depth)
{
    static const char *const nums[] = {"0", "1", "-1", "2", "3", "4", "5", "10", "11", "15", "64", "100", "255", "256",
                                       "16777215", "2147483647", "2147483648", "-2147483649", "9223372036854775807",
                                       "99999999999999999999999", "1.5", "-0", "1e5", "1700000000000"};

    switch (depth > 6 ? 2 + rn(4) : rn(10)) {
    case 0:
    case 1:
    case 6:
        sb_addn(o, "{", 1);
        for (unsigned i = 0, n = rn(8); i < n; i++) {
            if (i)
                sb_addn(o, ",", 1);
            sb_addn(o, "\"", 1);
            sb_add(o, k_keys[rn(COUNT(k_keys))]);
            sb_addn(o, "\":", 2);
            gen_value(o, depth + 1);
        }
        sb_addn(o, "}", 1);
        break;
    case 2:
    case 3:
        gen_string(o);
        break;
    case 4:
        sb_add(o, nums[rn(COUNT(nums))]);
        break;
    case 5:
        sb_add(o, rn(3) == 0 ? "null" : rn(2) ? "true" : "false");
        break;
    default:
        sb_addn(o, "[", 1);
        for (unsigned i = 0, n = rn(depth < 3 ? 12 : 4); i < n; i++) {
            if (i)
                sb_addn(o, ",", 1);
            gen_value(o, depth + 1);
        }
        sb_addn(o, "]", 1);
        break;
    }
}

/* ---- Seeds with values replaced ---- */

static json_t g_values[20000];
static int g_nvalues;

static void collect(json_t v, int depth)
{
    json_iter_t it;
    json_t k, x;

    if (g_nvalues < (int)COUNT(g_values))
        g_values[g_nvalues++] = v;
    if (depth > 60)
        return;
    json_iter(v, &it);
    while (json_next(&it, &k, &x))
        collect(x, depth + 1);
}

static void mutated_seed(sb_t *doc)
{
    sb_t cur = {0};
    json_t v;
    unsigned s = rn(COUNT(k_seeds));

    sb_addn(&cur, k_seeds[s].s, k_seeds[s].n);
    for (unsigned r = 0, rounds = 1 + rn(5); r < rounds; r++) {
        sb_t rep = {0}, next = {0};
        json_t t;
        if (!json_parse(cur.data, cur.len, &v))
            fail("a seed or its mutation is not valid JSON", cur.data);
        g_nvalues = 0;
        collect(v, 0);
        t = g_values[rn(g_nvalues)];
        switch (rn(6)) {
        case 0:
        case 1:
            gen_value(&rep, 3);
            break;
        case 2: { /* a value from another seed */
            unsigned o = rn(COUNT(k_seeds));
            json_t w, q;
            json_parse(k_seeds[o].s, k_seeds[o].n, &w);
            g_nvalues = 0;
            collect(w, 0);
            q = g_values[rn(g_nvalues)];
            sb_addn(&rep, q.p, (size_t)(q.end - q.p));
            break;
        }
        case 3: {
            static const char *const z[] = {"null", "[]", "{}", "\"\"", "0", "-1", "true", "\"x\""};
            sb_add(&rep, z[rn(COUNT(z))]);
            break;
        }
        case 4: /* repeated, which is only valid inside an array: rejected below otherwise */
            sb_addn(&rep, t.p, (size_t)(t.end - t.p));
            sb_addn(&rep, ",", 1);
            sb_addn(&rep, t.p, (size_t)(t.end - t.p));
            break;
        default: {
            char id[16];
            int n = snprintf(id, sizeof id, "\"%u\"", rn(1000));
            sb_addn(&rep, id, (size_t)n);
            break;
        }
        }
        sb_addn(&next, cur.data, (size_t)(t.p - cur.data));
        sb_addn(&next, rep.data, rep.len);
        sb_addn(&next, t.end, (size_t)(cur.data + cur.len - t.end));
        if (json_parse(next.data, next.len, &v)) {
            sb_free(&cur);
            cur = next;
        } else {
            sb_free(&next);
        }
        sb_free(&rep);
    }
    *doc = cur;
}

static void any_doc(sb_t *doc, json_t *v)
{
    if (rn(4))
        mutated_seed(doc);
    else
        gen_value(doc, 0);
    if (!json_parse(doc->data, doc->len, v))
        fail("generated JSON does not parse", doc->data);
}

/* ---- Targets ---- */

static void walk(json_t v, int depth)
{
    json_iter_t it;
    json_t k, x;
    sb_t s = {0};
    long long n;
    char raw[32];

    json_str(v, &s);
    sb_free(&s);
    json_int(v, &n);
    json_raw(v, raw, sizeof raw);
    if (depth > 70)
        return;
    json_iter(v, &it);
    while (json_next(&it, &k, &x))
        walk(x, depth + 1);
}

static void fuzz_json(void)
{
    static const char *const events[] = {
        "CHANNEL_CREATE", "CHANNEL_DELETE", "CHANNEL_UPDATE", "GUILD_CREATE", "GUILD_DELETE", "GUILD_EMOJIS_UPDATE",
        "GUILD_MEMBER_UPDATE", "GUILD_ROLE_CREATE", "GUILD_ROLE_DELETE", "GUILD_ROLE_UPDATE", "GUILD_STICKERS_UPDATE",
        "GUILD_UPDATE", "THREAD_CREATE", "THREAD_DELETE", "THREAD_UPDATE", "USER_GUILD_SETTINGS_UPDATE",
        "USER_SETTINGS_UPDATE", "THREAD_MEMBER_UPDATE", "THREAD_MEMBERS_UPDATE"};
    sb_t doc = {0};
    json_t v, e;
    model_t *m;

    any_doc(&doc, &v);
    walk(v, 0);
    {
        msg_t msg = {0};
        msg_parse(v, &msg);
        msg_free(&msg);
    }
    msg_batch_free(msg_batch_from_array(v, BATCH_HISTORY, "1", 50));
    msg_batch_free(msg_batch_search(v));
    msg_batch_free(msg_batch_reaction(v, rn(2) ? 1 : -1, "123"));
    msg_batch_free(msg_batch_poll_vote(v, 1, "123"));
    msg_batch_free(msg_batch_one(v, BATCH_NEW));
    {
        profile_t p = {0};
        profile_parse(v, rn(2) ? "5" : NULL, &p);
        profile_free(&p);
    }
    {
        ml_t l = {0};
        sb_t d2 = {0}, act = {0};
        ml_apply(&l, v);
        any_doc(&d2, &e);
        ml_apply(&l, e);
        sb_free(&d2);
        ml_free(&l);
        ml_activity(v, &act);
        sb_free(&act);
        ml_status(v);
    }
    m = model_from_ready(v);
    for (int r = 0; r < 4; r++) {
        sb_t d2 = {0};
        model_t *next;
        any_doc(&d2, &e);
        next = model_apply(m, events[rn(COUNT(events))], e);
        if (next) {
            model_free(m);
            m = next;
        }
        sb_free(&d2);
    }
    for (unsigned i = 0; i < m->nchannels; i++) {
        model_notify(m, i);
        model_muted(m, i, 1700000000000ll);
        model_unread(m, i);
        model_channel_guild(m, i);
    }
    for (unsigned g = 0; g < m->nguilds; g++) {
        unsigned c = 0;
        model_role_t role;
        model_emoji_t em;
        if (m->guilds[g].first + m->guilds[g].count > m->nchannels)
            fail("a server's channels run past the channel list", doc.data);
        if (m->guilds[g].hidden_first + m->guilds[g].hidden_count > m->nhidden)
            fail("a server's hidden channels run past the hidden list", doc.data);
        if (m->guilds[g].folder >= (int)m->nfolders)
            fail("a server's folder is not in the folder list", doc.data);
        while (model_role_next(m, (int)g, &c, &role))
            ;
        for (c = 0; model_emoji_next(m, (int)g, &c, &em);)
            ;
        for (c = 0; model_sticker_next(m, (int)g, &c, &em);)
            ;
        model_role_color(m, (int)g, "1,2,3");
        model_has_role(m, (int)g, "1");
        model_guild_muted(m, (int)g, 0);
    }
    model_free(m);
    sb_free(&doc);
}

static void fuzz_command(void)
{
    static const char *const toks[] = {" ", "  ", ":", "true", "no", "YES", "12", "-3", "3.5", "1.2.3", "<@123456>",
                                       "<#45678>", "<@&6>", "abc", "\"", "\xc3\xa9", "x:y"};
    static json_t objs[512];
    static char words[64][40];
    int nobjs = 0, nwords = 0;
    sb_t doc = {0};
    json_t v;

    any_doc(&doc, &v);
    g_nvalues = 0;
    collect(v, 0);
    for (int i = 0; i < g_nvalues; i++) {
        if (json_type(g_values[i]) == JSON_OBJECT && nobjs < (int)COUNT(objs))
            objs[nobjs++] = g_values[i];
        if (json_type(g_values[i]) == JSON_STRING && nwords < (int)COUNT(words))
            json_raw(g_values[i], words[nwords++], sizeof words[0]);
    }
    for (int r = 0; nobjs && r < 8; r++) {
        char args[512], err[128];
        size_t n = 0, used;
        json_t cmd = objs[rn(nobjs)], opts, it_cmd;
        json_iter_t it = {0};
        sb_t out = {0};
        for (unsigned i = 0, k = rn(12); i < k; i++) {
            const char *t = nwords && rn(2) ? words[rn(nwords)] : toks[rn(COUNT(toks))];
            size_t l = strlen(t);
            if (n + l + 2 >= sizeof args)
                break;
            memcpy(args + n, t, l);
            n += l;
            if (rn(3) == 0)
                args[n++] = ':';
            else if (rn(2))
                args[n++] = ' ';
        }
        args[n] = 0;
        cmd_leaf(cmd, args, n, &opts, &used);
        if (used > n)
            fail("cmd_leaf used more than the arguments", args);
        if (cmd_build(cmd, args, n, &out, err, 1 + rn(sizeof err - 1))) {
            json_t check;
            if (!json_parse(out.data, out.len, &check))
                fail("cmd_build made invalid JSON", out.data);
        }
        sb_free(&out);
        while (cmd_next(v, &it, &it_cmd))
            ;
        cmd_find(v, args, n, &it_cmd);
        cmd_app(v, "1", &it_cmd);
    }
    sb_free(&doc);
}

static const char *custom_emoji(void *ctx, const char *name, size_t n)
{
    (void)ctx, (void)name;
    return n == 3 ? "<:abc:123>" : NULL;
}

static void fuzz_text(void)
{
    static const char *const pieces[] = {
        "**", "__", "*", "_", "~~", "||", "`", "``", "```", "```c\n", "> ", ">>> ", "# ", "## ", "### ", "-# ", "- ",
        "* ", "1. ", "\n", "\n\n", "[a](https://x.y)", "[", "](", "https://a.b/c", ")", "<https://q>",
        "<t:1700000000:R>", "<t:", "\\", "\\*", ":smile:", ":abc:", "::", "hello", " ", "\xee\x80\x80",
        "\xee\x80\x81", "\xee\x80\x82", "a123:nm", "\xee\x80\x83", "\xee\x80\x84", "\xf0\x9f\x98\x80", "\xc3",
        "\xe2\x80", "\xff", "\xc3\xa9", "from:", "has:image", "in:gen", "before:2026-09-01", "after:",
        "during:2026-02-30", "pinned:true", "@x", "#y", "\"q\""};
    char buf[4096], small[64];
    size_t n = 0;

    for (unsigned i = 0, k = rn(60); i < k; i++) {
        const char *p = pieces[rn(COUNT(pieces))];
        size_t l = strlen(p);
        if (n + l >= sizeof buf)
            break;
        memcpy(buf + n, p, l);
        n += l;
    }
    buf[n] = 0;
    {
        md_doc_t d = {0};
        md_parse(buf, n, &d);
        for (int i = 0; i < d.nspans; i++) {
            if (d.spans[i].start < 0 || d.spans[i].len < 0 || d.spans[i].start + d.spans[i].len > d.len)
                fail("markdown span out of the text", buf);
            if (d.spans[i].link >= d.nlinks)
                fail("markdown span links past the links", buf);
            if (d.spans[i].link >= 0)
                md_link(&d, d.spans[i].link);
        }
        for (int i = 0; i < d.nblocks; i++)
            if (d.blocks[i].start < 0 || d.blocks[i].len < 0 || d.blocks[i].start + d.blocks[i].len > d.len)
                fail("markdown block out of the text", buf);
        md_free(&d);
    }
    {
        search_filter_t f[8];
        sb_t content = {0};
        search_parse(buf, f, 1 + (int)rn(8), &content);
        sb_free(&content);
    }
    {
        sb_t out = {0};
        emoji_expand(buf, n, &out, custom_emoji, NULL);
        sb_free(&out);
        b64_decode(buf, n, &out);
        sb_free(&out);
    }
    lstrcpynA(small, buf, sizeof small);
    msg_iso_ms(small);
    snowflake_ms(small);
    search_day_snowflake(small, (int)rn(2));
    emoji_by_name(small, strlen(small));
    {
        static qr_t qr;
        qr_encode(buf, n, &qr);
    }
}

static void fuzz_inflate(void)
{
    static unsigned char src[200000], packed[260000];
    static inflate_t z;
    z_stream s = {0};

    /* One zlib stream, several sync-flushed messages, matches reaching into earlier ones. */
    deflateInit2(&s, (int)rn(10), Z_DEFLATED, 15, 8, (int)rn(4));
    inflate_init(&z);
    for (unsigned m = 0, msgs = 1 + rn(6); m < msgs; m++) {
        size_t n = rn(3) == 0 ? rn(100000) : rn(3000), got;
        sb_t out = {0};
        for (size_t i = 0; i < n; i++)
            src[i] = rn(4) ? (unsigned char)"abcdefgh {}\":,"[rn(14)] : (unsigned char)rnd();
        if (rn(3) == 0)
            for (size_t i = 50; i < n; i++)
                src[i] = src[i - 1 - rn(40)];
        s.next_in = src;
        s.avail_in = (uInt)n;
        s.next_out = packed;
        s.avail_out = sizeof packed;
        deflate(&s, Z_SYNC_FLUSH);
        got = sizeof packed - s.avail_out;
        if (!inflate_complete(packed, got))
            fail("zlib's sync flush is not seen as complete", NULL);
        if (!inflate_message(&z, packed, got, &out) || out.len != n || (n && memcmp(out.data, src, n)))
            fail("inflate differs from zlib", NULL);
        sb_free(&out);
    }
    deflateEnd(&s);

    /* Corrupted streams must fail or produce something, never misbehave. */
    {
        size_t n = 1 + rn(5000), got;
        z_stream t = {0};
        sb_t out = {0};
        for (size_t i = 0; i < n; i++)
            src[i] = (unsigned char)"abcab"[rn(5)];
        deflateInit(&t, 6);
        t.next_in = src;
        t.avail_in = (uInt)n;
        t.next_out = packed;
        t.avail_out = sizeof packed;
        deflate(&t, Z_SYNC_FLUSH);
        got = sizeof packed - t.avail_out;
        deflateEnd(&t);
        for (unsigned k = 0, flips = 1 + rn(8); k < flips; k++)
            packed[rn(got)] ^= (unsigned char)(1u << rn(8));
        if (rn(2))
            got = 1 + rn(got);
        inflate_init(&z);
        inflate_message(&z, packed, got, &out);
        sb_free(&out);
    }
}

static void fuzz_apng(void)
{
    static unsigned char b[4096];
    size_t n = sizeof k_apng;
    apng_t *a;

    memcpy(b, k_apng, n);
    for (unsigned k = 0, edits = 1 + rn(8); k < edits; k++) {
        switch (rn(4)) {
        case 0:
            b[rn(n)] ^= (unsigned char)(1u << rn(8));
            break;
        case 1:
            b[rn(n)] = (unsigned char)rnd();
            break;
        case 2:
            n = 1 + rn(n);
            break;
        default:
            if (n < sizeof b) {
                size_t p = rn(n + 1);
                memmove(b + p + 1, b + p, n - p);
                b[p] = (unsigned char)rnd();
                n++;
            }
            break;
        }
    }
    a = apng_open(b, n);
    if (a) {
        unsigned w, h;
        apng_size(a, &w, &h);
        if ((size_t)w * h <= 1u << 22) {
            unsigned char *canvas = mem_alloc((size_t)w * h * 4 + 1);
            for (int i = 0; i < 6; i++)
                apng_next(a, canvas);
            mem_free(canvas);
        }
        apng_free(a);
    }
}

int main(int argc, char **argv)
{
    long iterations = argc > 1 ? atol(argv[1]) : 1000;
    const char *which = argc > 2 ? argv[2] : "all";
    int all = !strcmp(which, "all");

    if (argc > 3)
        g_rng = strtoull(argv[3], NULL, 10) | 1;
    for (long i = 0; i < iterations; i++) {
        if (all || !strcmp(which, "json"))
            fuzz_json();
        if (all || !strcmp(which, "command"))
            fuzz_command();
        if (all || !strcmp(which, "text"))
            fuzz_text();
        if ((all && i % 20 == 0) || !strcmp(which, "inflate"))
            fuzz_inflate();
        if (all || !strcmp(which, "apng"))
            fuzz_apng();
    }
    printf("fuzz: %ld iterations of %s, no findings\n", iterations, which);
    return 0;
}
