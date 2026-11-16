#ifndef BINK_RENDERER_H
#define BINK_RENDERER_H

/* Renderer interface shared by the SDL players. Each backend defines
 * BinkRenderer itself, so exactly one is linked per player. This header
 * deliberately includes no SDL, GL or Metal headers. */

#include "bink.h"

typedef struct BinkRenderer BinkRenderer;

typedef struct {
    int interp;   /* 0 presentation, 1 interpolation */
    int have_next;
    float t;
    unsigned long blocks_w;
    unsigned long blocks_h;
    int slot_cur; /* 0 or 1: the pair's current frame */
    int capture;  /* render for readback: do not present */
} BinkRenderParams;

typedef struct {
    const char *name;          /* "opengl", "metal" */
    unsigned int window_flags; /* SDL_WINDOW_OPENGL or SDL_WINDOW_METAL */
    /* Called before the window exists, for context attributes. */
    int (*prepare)(void);
    BinkRenderer *(*create)(void *window, unsigned long w, unsigned long h);
    int (*upload)(BinkRenderer *r, bink object, int slot);
    int (*upload_motion)(BinkRenderer *r, bink object);
    int (*draw)(BinkRenderer *r, const BinkRenderParams *params);
    /* Read the last drawn frame back as packed RGB24 at the plane size,
     * top-down. Returns 0 on success. */
    int (*readback)(BinkRenderer *r, unsigned char *rgb, unsigned long w,
                    unsigned long h);
    void (*destroy)(BinkRenderer *r);
} BinkRendererBackend;

#endif
