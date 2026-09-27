#pragma once
#include <windows.h>

/*
 * Anti-aliased shapes and images through the GDI+ flat API (gdiplus.dll).
 * Text stays on GDI for ClearType. Colors are 0xAARRGGBB.
 */
typedef struct gfx gfx_t;
typedef struct gfx_image gfx_image_t;

int gfx_init(void);
gfx_t *gfx_begin(HDC dc);
/* Call before drawing GDI text on the same DC. */
void gfx_flush(gfx_t *g);
void gfx_end(gfx_t *g);

void gfx_round_rect(gfx_t *g, int x, int y, int w, int h, int radius, unsigned argb);
void gfx_circle(gfx_t *g, int x, int y, int d, unsigned argb);
/* Draws `img` scaled into a w x h rounded rectangle (radius = w/2 for a circle). */
void gfx_image(gfx_t *g, gfx_image_t *img, int x, int y, int w, int h, int radius);

/* Decodes PNG, JPEG or GIF bytes. Safe from any thread. */
gfx_image_t *gfx_image_load(const void *data, size_t n);
void gfx_image_free(gfx_image_t *img);
