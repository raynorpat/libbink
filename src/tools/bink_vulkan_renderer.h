#ifndef BINK_VULKAN_RENDERER_H
#define BINK_VULKAN_RENDERER_H

#include "tools/bink_renderer.h"

int bink_vulkan_prepare(void);
BinkRenderer *bink_vulkan_renderer_create(void *window, unsigned long w,
                                          unsigned long h);
int bink_vulkan_renderer_upload(BinkRenderer *r, bink object, int slot);
int bink_vulkan_renderer_upload_motion(BinkRenderer *r, bink object,
                                       unsigned long *blocks_w,
                                       unsigned long *blocks_h);
int bink_vulkan_renderer_draw(BinkRenderer *r, const BinkRenderParams *params);
int bink_vulkan_renderer_readback(BinkRenderer *r, unsigned char *rgb,
                                  unsigned long w, unsigned long h);
void bink_vulkan_renderer_destroy(BinkRenderer *r);

#endif
