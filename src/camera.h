#pragma once
#include <wchar.h>
#include "vp8.h"

/*
 * Webcams through Media Foundation: a capture thread asks the camera for
 * NV12 at a size and rate (Windows scales and converts as needed) and
 * hands each picture on as I420.
 */

#define CAMERA_NAME 64

/* A picture, on the capture thread. */
typedef void (*camera_frame_fn)(void *ctx, const vp8_image_t *img, unsigned long long ms);

/* The cameras' names, in index order; returns how many. */
int camera_list(wchar_t (*names)[CAMERA_NAME], int max);
/* Opens camera `index` (0 for the first) at about w x h and `fps`. */
int camera_start(int index, int w, int h, int fps, camera_frame_fn frame, void *ctx);
void camera_stop(void);
