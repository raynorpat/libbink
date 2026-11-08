/*
 * Bink DSP routines
 * Copyright (c) 2009 Konstantin Shishkov
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "bink_dsp.h"

#include <string.h>

#define A1  2896 /* (1/sqrt(2))<<12 */
#define A2  2217
#define A3  3784
#define A4 -5352

#define MUL(X, Y) ((int)((unsigned)(X) * (Y)) >> 11)

#define IDCT_TRANSFORM(dest, s0, s1, s2, s3, s4, s5, s6, s7, \
                       d0, d1, d2, d3, d4, d5, d6, d7, munge, src) { \
    const int a0 = (src)[s0] + (src)[s4]; \
    const int a1 = (src)[s0] - (src)[s4]; \
    const int a2 = (src)[s2] + (src)[s6]; \
    const int a3 = MUL(A1, (src)[s2] - (src)[s6]); \
    const int a4 = (src)[s5] + (src)[s3]; \
    const int a5 = (src)[s5] - (src)[s3]; \
    const int a6 = (src)[s1] + (src)[s7]; \
    const int a7 = (src)[s1] - (src)[s7]; \
    const int b0 = a4 + a6; \
    const int b1 = MUL(A3, a5 + a7); \
    const int b2 = MUL(A4, a5) - b0 + b1; \
    const int b3 = MUL(A1, a6 - a4) - b2; \
    const int b4 = MUL(A2, a7) + b3 - b1; \
    (dest)[d0] = munge(a0 + a2 + b0); \
    (dest)[d1] = munge(a1 + a3 - a2 + b2); \
    (dest)[d2] = munge(a1 - a3 + a2 + b3); \
    (dest)[d3] = munge(a0 - a2 - b4); \
    (dest)[d4] = munge(a0 - a2 + b4); \
    (dest)[d5] = munge(a1 - a3 + a2 - b3); \
    (dest)[d6] = munge(a1 + a3 - a2 - b2); \
    (dest)[d7] = munge(a0 + a2 - b0); \
}

#define MUNGE_NONE(x) (x)
#define IDCT_COL(dest, src) \
    IDCT_TRANSFORM(dest, 0, 8, 16, 24, 32, 40, 48, 56, \
                   0, 8, 16, 24, 32, 40, 48, 56, MUNGE_NONE, src)

#define MUNGE_ROW(x) (((x) + 0x7F) >> 8)
#define IDCT_ROW(dest, src) \
    IDCT_TRANSFORM(dest, 0, 1, 2, 3, 4, 5, 6, 7, \
                   0, 1, 2, 3, 4, 5, 6, 7, MUNGE_ROW, src)

static void bink_idct_col(int *dest, const int32_t *src)
{
    if ((src[8] | src[16] | src[24] | src[32] | src[40] | src[48] |
         src[56]) == 0) {
        dest[0] = src[0];
        dest[8] = src[0];
        dest[16] = src[0];
        dest[24] = src[0];
        dest[32] = src[0];
        dest[40] = src[0];
        dest[48] = src[0];
        dest[56] = src[0];
    } else {
        IDCT_COL(dest, src);
    }
}

static void bink_idct_c(int32_t *block)
{
    int i;
    int temp[64];

    for (i = 0; i < 8; i++)
        bink_idct_col(&temp[i], &block[i]);
    for (i = 0; i < 8; i++)
        IDCT_ROW((&block[8 * i]), (&temp[8 * i]));
}

static void bink_idct_add_c(unsigned char *dest, int linesize,
                            int32_t *block)
{
    int i, j;

    bink_idct_c(block);
    for (i = 0; i < 8; i++, dest += linesize, block += 8)
        for (j = 0; j < 8; j++)
            dest[j] = (unsigned char)(dest[j] + block[j]);
}

static void bink_idct_put_c(unsigned char *dest, int linesize,
                            int32_t *block)
{
    int i;
    int temp[64];

    for (i = 0; i < 8; i++)
        bink_idct_col(&temp[i], &block[i]);
    for (i = 0; i < 8; i++)
        IDCT_ROW((&dest[i * linesize]), (&temp[8 * i]));
}

static void scale_block_c(const unsigned char src[64], unsigned char *dst,
                          int linesize)
{
    int i, j;
    uint16_t *dst1 = (uint16_t *)dst;
    uint16_t *dst2 = (uint16_t *)(dst + linesize);

    for (j = 0; j < 8; j++) {
        for (i = 0; i < 8; i++)
            dst1[i] = dst2[i] = (uint16_t)(src[i] * 0x0101);
        src += 8;
        dst1 += linesize;
        dst2 += linesize;
    }
}

static void add_pixels8_c(unsigned char *pixels, const int16_t *block,
                          int line_size)
{
    int i;

    for (i = 0; i < 8; i++) {
        pixels[0] = (unsigned char)(pixels[0] + block[0]);
        pixels[1] = (unsigned char)(pixels[1] + block[1]);
        pixels[2] = (unsigned char)(pixels[2] + block[2]);
        pixels[3] = (unsigned char)(pixels[3] + block[3]);
        pixels[4] = (unsigned char)(pixels[4] + block[4]);
        pixels[5] = (unsigned char)(pixels[5] + block[5]);
        pixels[6] = (unsigned char)(pixels[6] + block[6]);
        pixels[7] = (unsigned char)(pixels[7] + block[7]);
        pixels += line_size;
        block += 8;
    }
}

void bink_idct_put(unsigned char *dst, int stride, int32_t block[64])
{
    bink_idct_put_c(dst, stride, block);
}

void bink_idct_add(unsigned char *dst, int stride, int32_t block[64])
{
    bink_idct_add_c(dst, stride, block);
}

void bink_scale_block(const unsigned char src[64], unsigned char *dst,
                      int stride)
{
    scale_block_c(src, dst, stride);
}

void bink_add_pixels8(unsigned char *dst, const int16_t block[64], int stride)
{
    add_pixels8_c(dst, block, stride);
}

void bink_fill_block(unsigned char *dst, int stride, int size,
                     unsigned char value)
{
    int i;

    for (i = 0; i < size; i++) {
        memset(dst, value, (size_t)size);
        dst += stride;
    }
}

void bink_copy_block(unsigned char *dst, const unsigned char *src, int stride)
{
    int i;

    for (i = 0; i < 8; i++) {
        memcpy(dst, src, 8);
        dst += stride;
        src += stride;
    }
}
