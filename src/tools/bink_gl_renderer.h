#ifndef BINK_GL_RENDERER_H
#define BINK_GL_RENDERER_H

#include "tools/bink_renderer.h"

int bink_gl_prepare(void);
BinkRenderer *bink_gl_renderer_create(void *window, unsigned long w,
                                      unsigned long h);
int bink_gl_renderer_upload(BinkRenderer *r, bink object, int slot);
int bink_gl_renderer_upload_motion(BinkRenderer *r, bink object);
int bink_gl_renderer_draw(BinkRenderer *r, const BinkRenderParams *params);
int bink_gl_renderer_readback(BinkRenderer *r, unsigned char *rgb,
                              unsigned long w, unsigned long h);
void bink_gl_renderer_destroy(BinkRenderer *r);

#endif
