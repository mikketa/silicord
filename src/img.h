#pragma once
#include <windows.h>
#include "sb.h"

/*
 * Background image loader for CDN icons and avatars. Each finished request
 * posts `msg` to the window: wParam is a r_image_t* (NULL on failure) and
 * lParam an sb_t* holding the key; the receiver frees both.
 */
void img_init(HWND wnd, UINT msg);
void img_request(const char *key, const char *cdn_path);
/* Drops pending requests; results of requests already running are discarded. */
void img_clear(void);
