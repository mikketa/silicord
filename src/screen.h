#pragma once
#include <windows.h>

/*
 * The screen, through DXGI Desktop Duplication: a capture thread copies
 * the monitor showing a window, draws the mouse pointer on it, and hands
 * each picture on as BGRA, at most `fps` times a second (a still screen at
 * least once a second, so a stream never goes quiet).
 */

/* A picture, on the capture thread; its rows are `stride` pixels apart. */
typedef void (*screen_frame_fn)(void *ctx, const unsigned *bgra, int w, int h, int stride, unsigned long long ms);

/* Starts capturing the monitor that shows `wnd`; returns 0 when it cannot be duplicated. */
int screen_start(HWND wnd, int fps, screen_frame_fn frame, void *ctx);
void screen_stop(void);
