/* Received-bytes counters and their wording. */
#include <windows.h>
#include "test.h"
#include "stats.h"

void entry(void)
{
    sb_t s = {0};

    stats_format(&s, 1000);
    check(str_eq(&s, "0 bytes received: gateway 0 bytes, API 0 bytes, media 0 bytes"), "nothing yet");
    sb_clear(&s);
    stats_add(STAT_GATEWAY, 812 * 1024);
    stats_add(STAT_GATEWAY_JSON, 5432 * 1024);
    stats_add(STAT_API, 96 * 1024);
    stats_add(STAT_CDN, 3400 * 1024);
    stats_add(STAT_COUNT, 1); /* ignored */
    check(stats_get(STAT_API) == 96 * 1024 && stats_get(STAT_COUNT) == 0, "counters add up");
    stats_format(&s, 4 * 3600000ull);
    check(str_eq(&s, "4.2 MB received (1.0 MB per hour): gateway 812 KB (5.3 MB inflated), API 96 KB, media 3.3 MB"),
          "totals and the hourly rate");
    sb_clear(&s);
    stats_format_cpu(&s, 432, 2 * 3600000ull);
    check(str_eq(&s, "0.4 s of CPU in 2.0 h (0.00%)"), "idle CPU over hours");
    sb_clear(&s);
    stats_format_cpu(&s, 12345, 30 * 60000ull);
    check(str_eq(&s, "12.3 s of CPU in 30 min (0.68%)"), "busy CPU over minutes");
    sb_free(&s);
    finish();
}
