#ifndef BINK_D3D_RENDERER_H
#define BINK_D3D_RENDERER_H

/* Renderer interface shared by the D3D9 and D3D11 Windows players. Each
 * implementation defines BinkD3DRenderer itself, so exactly one renderer
 * is linked into a player. */

#include "bink.h"

typedef struct BinkD3DRenderer BinkD3DRenderer;

typedef struct {
    int interp;   /* 0 presentation, 1 interpolation */
    int have_next;
    float t;
    unsigned long blocks_w;
    unsigned long blocks_h;
    int slot_cur; /* 0 or 1: the pair's current frame */
} BinkD3DDraw;

/* hwnd is an HWND passed as void * so this header stays free of windows.h. */
BinkD3DRenderer *bink_d3d9_renderer_create(void *hwnd, unsigned long w,
                                           unsigned long h);
BinkD3DRenderer *bink_d3d11_renderer_create(void *hwnd, unsigned long w,
                                            unsigned long h);

/* Upload the current frame's planes into slot (0 or 1). */
int bink_d3d_renderer_upload(BinkD3DRenderer *r, bink object, int slot);
/* Upload the current frame's motion field (+128 per byte). */
int bink_d3d_renderer_upload_motion(BinkD3DRenderer *r, bink object);
int bink_d3d_renderer_draw(BinkD3DRenderer *r, const BinkD3DDraw *draw);
void bink_d3d_renderer_destroy(BinkD3DRenderer *r);

#endif
