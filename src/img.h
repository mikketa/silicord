#pragma once
#include <windows.h>
#include "sb.h"
#include "render.h"

/*
 * Background image loader for CDN icons and avatars. Each finished request
 * posts `msg` to the window: wParam is a r_image_t* (NULL on failure) and
 * lParam an sb_t* holding the key; the receiver frees both.
 */
void img_init(HWND wnd, UINT msg);
/* Queues a download; the image is decoded to at most max_px on its longest side. */
void img_request(const char *key, const char *cdn_path, int max_px);
/*
 * Decodes an image from the disk cache right away (NULL if it is not there).
 * Used on the UI thread so images seen before appear in the same frame.
 */
r_image_t *img_cached(const char *cdn_path, int max_px);
/* Drops pending requests; results of requests already running are discarded. */
void img_clear(void);
