#ifndef LIBBINK_DSP_H
#define LIBBINK_DSP_H

#include <stdint.h>

/** Inverse DCT applied to an 8x8 destination block. */
void bink_idct_put(unsigned char *dst, int stride, int32_t block[64]);
/** Inverse DCT added to an existing 8x8 destination block. */
void bink_idct_add(unsigned char *dst, int stride, int32_t block[64]);
/** Expand an 8x8 block to 16x16 by duplicating pixels in both axes. */
void bink_scale_block(const unsigned char src[64], unsigned char *dst,
                      int stride);
/** Add signed 8x8 residue with byte wraparound. */
void bink_add_pixels8(unsigned char *dst, const int16_t block[64], int stride);
/** Fill a size x size byte block. */
void bink_fill_block(unsigned char *dst, int stride, int size,
                     unsigned char value);
/** Copy an 8x8 byte block. */
void bink_copy_block(unsigned char *dst, const unsigned char *src, int stride);

#endif
