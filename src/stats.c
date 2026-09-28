#include <windows.h>
#include "stats.h"

static volatile LONG64 g_bytes[STAT_COUNT];

void stats_add(int kind, unsigned long long bytes)
{
    if (kind >= 0 && kind < STAT_COUNT)
        InterlockedAdd64(&g_bytes[kind], (LONG64)bytes);
}

unsigned long long stats_get(int kind)
{
    return kind >= 0 && kind < STAT_COUNT ? (unsigned long long)InterlockedAdd64(&g_bytes[kind], 0) : 0;
}

/* "812 bytes", "96 KB", "5.3 MB", "1.2 GB": one decimal from MB up, like Windows. */
static void add_size(sb_t *out, unsigned long long n)
{
    static const char *const units[] = {"KB", "MB", "GB", "TB"};
    unsigned long long whole = n, tenths = 0;
    int u = -1;

    if (n < 1024) {
        sb_u64(out, n);
        sb_add(out, n == 1 ? " byte" : " bytes");
        return;
    }
    while (whole >= 1024 && u < 3) {
        tenths = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        u++;
    }
    sb_u64(out, whole);
    if (u > 0 && whole < 100) {
        sb_add(out, ".");
        sb_u64(out, tenths);
    }
    sb_add(out, " ");
    sb_add(out, units[u]);
}

void stats_format(sb_t *out, unsigned long long uptime_ms)
{
    unsigned long long gw = stats_get(STAT_GATEWAY), json = stats_get(STAT_GATEWAY_JSON), api = stats_get(STAT_API),
                       cdn = stats_get(STAT_CDN), total = gw + api + cdn;

    add_size(out, total);
    sb_add(out, " received");
    if (uptime_ms >= 60000) { /* a rate over less than a minute says little */
        sb_add(out, " (");
        add_size(out, total * 3600000ull / uptime_ms);
        sb_add(out, " per hour)");
    }
    sb_add(out, ": gateway ");
    add_size(out, gw);
    if (json) {
        sb_add(out, " (");
        add_size(out, json);
        sb_add(out, " inflated)");
    }
    sb_add(out, ", API ");
    add_size(out, api);
    sb_add(out, ", media ");
    add_size(out, cdn);
}

void stats_format_cpu(sb_t *out, unsigned long long cpu_ms, unsigned long long uptime_ms)
{
    unsigned long long hundredths = uptime_ms ? cpu_ms * 10000ull / uptime_ms : 0; /* per cent, times 100 */

    sb_u64(out, cpu_ms / 1000);
    sb_add(out, ".");
    sb_u64(out, cpu_ms % 1000 / 100);
    sb_add(out, " s of CPU in ");
    if (uptime_ms < 3600000ull) {
        sb_u64(out, uptime_ms / 60000);
        sb_add(out, " min");
    } else {
        sb_u64(out, uptime_ms / 3600000ull);
        sb_add(out, ".");
        sb_u64(out, uptime_ms % 3600000ull / 360000ull);
        sb_add(out, " h");
    }
    sb_add(out, " (");
    sb_u64(out, hundredths / 100);
    sb_add(out, ".");
    if (hundredths % 100 < 10)
        sb_add(out, "0");
    sb_u64(out, hundredths % 100);
    sb_add(out, "%)");
}
