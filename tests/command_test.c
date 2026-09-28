/* Slash command parsing tests. */
#include <windows.h>
#include "test.h"
#include "command.h"

static const char k_index[] =
    "{\"applications\":[{\"id\":\"9\",\"name\":\"Bot\",\"icon\":\"abc\"}],"
    "\"application_commands\":["
    "{\"id\":\"1\",\"application_id\":\"9\",\"version\":\"5\",\"type\":1,\"name\":\"roll\",\"description\":\"Roll\","
    "\"options\":[{\"type\":4,\"name\":\"sides\",\"description\":\"d\",\"required\":true},"
    "{\"type\":5,\"name\":\"secret\",\"description\":\"s\"}]},"
    "{\"id\":\"2\",\"application_id\":\"9\",\"version\":\"6\",\"type\":1,\"name\":\"say\",\"description\":\"Say\","
    "\"options\":[{\"type\":3,\"name\":\"text\",\"description\":\"t\",\"required\":true}]},"
    "{\"id\":\"3\",\"application_id\":\"9\",\"version\":\"7\",\"type\":1,\"name\":\"config\",\"description\":\"C\","
    "\"options\":[{\"type\":2,\"name\":\"user\",\"description\":\"u\",\"options\":["
    "{\"type\":1,\"name\":\"set\",\"description\":\"s\",\"options\":["
    "{\"type\":6,\"name\":\"who\",\"description\":\"w\",\"required\":true},"
    "{\"type\":3,\"name\":\"mode\",\"description\":\"m\",\"choices\":[{\"name\":\"Fast\",\"value\":\"f\"}]}]}]}]},"
    "{\"id\":\"4\",\"application_id\":\"9\",\"version\":\"8\",\"type\":2,\"name\":\"Report\"}]}";

static int contains(const sb_t *s, const char *needle)
{
    size_t n = sc_strlen(needle);

    for (size_t i = 0; i + n <= s->len; i++) {
        size_t k = 0;
        while (k < n && s->data[i + k] == needle[k])
            k++;
        if (k == n)
            return 1;
    }
    return 0;
}

static int build(json_t index, const char *name, const char *args, sb_t *out, char *err)
{
    json_t cmd;

    sb_clear(out);
    return cmd_find(index, name, sc_strlen(name), &cmd) && cmd_build(cmd, args, sc_strlen(args), out, err, 128);
}

/* Names are up to 32 characters of any script: 64 bytes of Cyrillic here. */
#define CYR32 "\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4" \
              "\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4\xD0\xB4"

static void test_long_names(void)
{
    static const char index_json[] =
        "{\"application_commands\":[{\"id\":\"1\",\"version\":\"2\",\"type\":1,\"name\":\"" CYR32 "\","
        "\"options\":[{\"type\":1,\"name\":\"" CYR32 "\",\"options\":["
        "{\"type\":3,\"description\":\"no name\"},{\"type\":3,\"name\":\"" CYR32 "\",\"required\":true}]}]}]}";
    json_t index, check_json;
    sb_t out = {0};
    char err[128];

    json_parse(index_json, sizeof index_json - 1, &index);
    check(build(index, CYR32, CYR32 " " CYR32 ":hi", &out, err), "long unicode names are found and filled by name");
    check(contains(&out, "\"name\":\"" CYR32 "\",\"type\":1") &&
              contains(&out, "\"type\":1,\"name\":\"" CYR32 "\",\"options\":[{\"type\":3,\"name\":\"" CYR32 "\",\"value\":\"hi\"}]"),
          "long unicode names are sent whole");
    check(json_parse(out.data, out.len, &check_json), "the interaction data is valid JSON");
    check(build(index, CYR32, CYR32 " hi", &out, err) && contains(&out, "\"value\":\"hi\""),
          "an option without a name is skipped");
    sb_free(&out);
}

void entry(void)
{
    json_t index, cmd, opts, v;
    json_iter_t it = {0};
    sb_t out = {0};
    char err[128];
    size_t used;
    int n = 0;

    check(json_parse(k_index, sizeof k_index - 1, &index), "index parses");
    while (cmd_next(index, &it, &cmd))
        n++;
    check(n == 3, "only chat-input commands are listed");
    check(cmd_find(index, "ROLL", 4, &cmd) && !cmd_find(index, "Report", 6, &cmd), "found by name, any case");
    check(cmd_app(index, "9", &v), "application found");

    check(build(index, "roll", " 20", &out, err) &&
              contains(&out, "\"options\":[{\"type\":4,\"name\":\"sides\",\"value\":20}]") &&
              contains(&out, "\"version\":\"5\",\"id\":\"1\",\"name\":\"roll\""),
          "a lone value fills the first option");
    check(build(index, "roll", "secret:yes sides:6", &out, err) &&
              contains(&out, "{\"type\":4,\"name\":\"sides\",\"value\":6}") &&
              contains(&out, "{\"type\":5,\"name\":\"secret\",\"value\":true}"),
          "named options in any order");
    check(!build(index, "roll", "", &out, err) && err[0], "missing required option");
    check(!build(index, "roll", "sides:many", &out, err) && err[0], "integers are checked");
    check(build(index, "say", "hello there: friend", &out, err) &&
              contains(&out, "\"value\":\"hello there: friend\""),
          "strings keep their spaces and colons");
    check(build(index, "say", "text:\"quoted\"", &out, err) && contains(&out, "\"value\":\"\\\"quoted\\\"\""),
          "strings are escaped");
    check(build(index, "config", "user set who:<@!123456789> mode:fast", &out, err) &&
              contains(&out, "\"options\":[{\"type\":2,\"name\":\"user\",\"options\":[{\"type\":1,\"name\":\"set\","
                             "\"options\":[{\"type\":6,\"name\":\"who\",\"value\":\"123456789\"},"
                             "{\"type\":3,\"name\":\"mode\",\"value\":\"f\"}]}]}]"),
          "subcommands nest and choices map to their value");
    check(!build(index, "config", "user", &out, err) && err[0], "a subcommand is required");
    check(!build(index, "config", "user set who:1 mode:slow", &out, err) && err[0], "choices are enforced");

    cmd_find(index, "config", 6, &cmd);
    check(!cmd_leaf(cmd, "user ", 5, &opts, &used) && json_count(opts) == 1, "leaf lists subcommands to pick");
    check(cmd_leaf(cmd, "user set wh", 11, &opts, &used) && used == 8 && json_count(opts) == 2,
          "leaf finds the options after subcommands");
    test_long_names();
    sb_free(&out);
    finish();
}
