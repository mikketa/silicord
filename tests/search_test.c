/* Search filter parsing tests. */
#include <windows.h>
#include "test.h"
#include "search.h"

void entry(void)
{
    search_filter_t f[8];
    sb_t content = {0};
    int n = search_parse("hello from:@Ann has:image  world in:#general PINNED:true time:now", f, 8, &content);

    check(n == 4, "four filters");
    check(n == 4 && f[0].key == SF_FROM && lstrcmpA(f[0].value, "Ann") == 0 && f[1].key == SF_HAS &&
              lstrcmpA(f[1].value, "image") == 0 && f[2].key == SF_IN && lstrcmpA(f[2].value, "general") == 0 &&
              f[3].key == SF_PINNED && lstrcmpA(f[3].value, "true") == 0,
          "filters keep their values, without @ or #, any case");
    check(str_eq(&content, "hello world time:now"), "other words stay the search text");
    sb_clear(&content);
    check(search_parse("from: alone", f, 8, &content) == 0 && str_eq(&content, "from: alone"), "a filter needs a value");

    check(search_day_snowflake("2015-01-02", 0) == 86400000ull << 22, "a day's first snowflake");
    check(search_day_snowflake("2015-01-01", 1) == 86400000ull << 22, "the next day's");
    check(search_day_snowflake("yesterday", 0) == 0, "not a date");
    sb_free(&content);
    finish();
}
