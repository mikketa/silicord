#pragma once
#include <windows.h>

/*
 * Frames paced by the display: while something moves, `wnd` is sent `msg`
 * once per refresh of the monitor it is on (300 times a second on a 300 Hz
 * screen), instead of on a timer. Nothing runs while nothing moves: each
 * vsync_request() asks for one message, after the next refresh.
 */
void vsync_start(HWND wnd, UINT msg);
void vsync_request(void);
