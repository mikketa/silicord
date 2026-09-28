#pragma once
#include "sb.h"

/*
 * What Silicord receives, counted as it arrives: the roadmap's "bytes received
 * per hour", shown in --debug and on the About screen. Safe from any thread.
 */
enum {
    STAT_GATEWAY,      /* gateway messages as they come off the wire (compressed) */
    STAT_GATEWAY_JSON, /* the same, inflated */
    STAT_API,          /* REST response bodies */
    STAT_CDN,          /* images, fonts and other files */
    STAT_COUNT
};

void stats_add(int kind, unsigned long long bytes);
unsigned long long stats_get(int kind);
/* "4.2 MB received (1.1 MB per hour): gateway 812 KB (5.3 MB inflated), API 96 KB, media 3.3 MB". */
void stats_format(sb_t *out, unsigned long long uptime_ms);
/* "0.4 s of CPU in 2.0 h (0.01%)". */
void stats_format_cpu(sb_t *out, unsigned long long cpu_ms, unsigned long long uptime_ms);
