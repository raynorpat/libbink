#ifndef BINK_ENC_VIDEO_H
#define BINK_ENC_VIDEO_H

#include "bink_bits.h"

#include <stdint.h>

void bink_enc_rgb_to_yuv(const uint8_t *rgb, unsigned width, unsigned height,
                         uint8_t *y, uint8_t *u, uint8_t *v);

typedef struct {
    uint8_t *cur[3];
    uint8_t *ref[3];
    int stride[3];
    int alloc_h[3];
    int plane_w[3];
    int plane_h[3];
    int blocks_x[3];
    int blocks_y[3];
    int has_prev;
} BinkEncPlanes;

int bink_enc_planes_init(BinkEncPlanes *planes, unsigned long width,
                         unsigned long height);
void bink_enc_planes_free(BinkEncPlanes *planes);

/* Append Y, then V, then U. Planes are lossless on the bytes rgb_to_yuv made. */
int bink_enc_write_frame(BinkBitWriter *bw, BinkEncPlanes *planes,
                         const uint8_t *y, const uint8_t *u, const uint8_t *v,
                         int keyframe, int motion_range);

#endif
