#pragma once
#include <windows.h>

/*
 * The screen, through DXGI Desktop Duplication: a capture thread copies a
 * monitor, draws the mouse pointer on it, and hands each picture on as
 * BGRA, at most `fps` times a second (a still screen at least once a
 * second, so a stream never goes quiet).
 */

/* A picture, on the capture thread; its rows are `stride` pixels apart. */
typedef void (*screen_frame_fn)(void *ctx, const unsigned *bgra, int w, int h, int stride, unsigned long long ms);

/* A monitor that can be shared, where Windows places it. */
typedef struct {
    HMONITOR monitor;
    RECT rect;
    int primary;
} screen_info_t;

/* The monitors, left to right (then top to bottom); returns how many, up to max. */
int screen_list(screen_info_t *out, int max);
/* A still of monitor s at w x h as opaque BGRA, through GDI (for choosing one); returns 0 on failure. */
int screen_thumbnail(const screen_info_t *s, int w, int h, unsigned *out);
/* Starts capturing a monitor; returns 0 when it cannot be duplicated. */
int screen_start(HMONITOR monitor, int fps, screen_frame_fn frame, void *ctx);
void screen_stop(void);
