/*
 * Bink 1 bundle readers.
 *
 * Ported from FFmpeg libavcodec/bink.c.
 * Copyright (c) 2006-2007 Konstantin Shishkov
 * Copyright (c) 2007-2008 Peter Ross
 * Licensed under the GNU Lesser General Public License, version 2.1 or later.
 */

#include "bink_video.h"
#include "bink.h"
#include "bink_dsp.h"
#include "binkdata.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char bink_rlelens[4] = {4, 8, 12, 32};

/* Returns 1 when a bundle read should proceed, 0 when there is nothing to
 * read, and -1 on malformed input. */
static int bundle_count(BinkBits *bits, BinkBundle *b, int *t)
{
    if (b->cur_dec == NULL || b->cur_dec > b->cur_ptr)
        return 0;
    *t = bink_bits_read(bits, b->len);
    if (*t < 0)
        return -1;
    if (*t == 0) {
        b->cur_dec = NULL;
        return 0;
    }
    return 1;
}

static int read_sign(BinkBits *bits, int *sign)
{
    int bit = bink_bits_read1(bits);

    if (bit < 0)
        return -1;
    *sign = -bit;
    return 0;
}

int bink_bundle_read_runs(BinkBits *bits, BinkBundle *b)
{
    int t;
    int v;
    int r;
    unsigned char *dec_end;

    r = bundle_count(bits, b, &t);
    if (r <= 0)
        return r;
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end)
        return -1;
    if (bink_bits_left(bits) < 1)
        return -1;
    if (bink_bits_read1(bits)) {
        v = bink_bits_read(bits, 4);
        if (v < 0)
            return -1;
        memset(b->cur_dec, v, (size_t)t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end) {
            v = bink_tree_decode(bits, &b->tree);
            if (v < 0)
                return -1;
            *b->cur_dec++ = (unsigned char)v;
        }
    }
    return 0;
}

int bink_bundle_read_motion_values(BinkBits *bits, BinkBundle *b)
{
    int t;
    int v;
    int sign;
    int r;
    unsigned char *dec_end;

    r = bundle_count(bits, b, &t);
    if (r <= 0)
        return r;
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end)
        return -1;
    if (bink_bits_left(bits) < 1)
        return -1;
    if (bink_bits_read1(bits)) {
        v = bink_bits_read(bits, 4);
        if (v < 0)
            return -1;
        if (v) {
            if (read_sign(bits, &sign) != 0)
                return -1;
            v = (v ^ sign) - sign;
        }
        memset(b->cur_dec, v, (size_t)t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end) {
            v = bink_tree_decode(bits, &b->tree);
            if (v < 0)
                return -1;
            if (v) {
                if (read_sign(bits, &sign) != 0)
                    return -1;
                v = (v ^ sign) - sign;
            }
            *b->cur_dec++ = (unsigned char)v;
        }
    }
    return 0;
}

int bink_bundle_read_block_types(BinkBits *bits, BinkBundle *b, int version)
{
    int t;
    int v;
    int last = 0;
    int r;
    unsigned char *dec_end;

    r = bundle_count(bits, b, &t);
    if (r <= 0)
        return r;
    if (version == 'k') {
        t ^= 0xBB;
        if (t == 0) {
            b->cur_dec = NULL;
            return 0;
        }
    }
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end)
        return -1;
    if (bink_bits_left(bits) < 1)
        return -1;
    if (bink_bits_read1(bits)) {
        v = bink_bits_read(bits, 4);
        if (v < 0)
            return -1;
        memset(b->cur_dec, v, (size_t)t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end) {
            v = bink_tree_decode(bits, &b->tree);
            if (v < 0)
                return -1;
            if (v < 12) {
                last = v;
                *b->cur_dec++ = (unsigned char)v;
            } else {
                int run = bink_rlelens[v - 12];

                if (dec_end - b->cur_dec < run)
                    return -1;
                memset(b->cur_dec, last, (size_t)run);
                b->cur_dec += run;
            }
        }
    }
    return 0;
}

int bink_bundle_read_patterns(BinkBits *bits, BinkBundle *b)
{
    int t;
    int v;
    int r;
    unsigned char *dec_end;

    r = bundle_count(bits, b, &t);
    if (r <= 0)
        return r;
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end)
        return -1;
    while (b->cur_dec < dec_end) {
        int high;

        if (bink_bits_left(bits) < 2)
            return -1;
        v = bink_tree_decode(bits, &b->tree);
        if (v < 0)
            return -1;
        high = bink_tree_decode(bits, &b->tree);
        if (high < 0)
            return -1;
        *b->cur_dec++ = (unsigned char)(v | (high << 4));
    }
    return 0;
}

int bink_bundle_read_colors(BinkBits *bits, BinkBundle *b,
                            BinkTree col_high[16], int *col_lastval,
                            int version)
{
    int t;
    int v;
    int high;
    int r;
    unsigned char *dec_end;

    r = bundle_count(bits, b, &t);
    if (r <= 0)
        return r;
    dec_end = b->cur_dec + t;
    if (dec_end > b->data_end)
        return -1;
    if (bink_bits_left(bits) < 1)
        return -1;
    if (bink_bits_read1(bits)) {
        high = bink_tree_decode(bits, &col_high[*col_lastval]);
        if (high < 0)
            return -1;
        *col_lastval = high;
        v = bink_tree_decode(bits, &b->tree);
        if (v < 0)
            return -1;
        v = (high << 4) | v;
        if (version < 'i') {
            int sign = ((signed char)v) >> 7;

            v = ((v & 0x7F) ^ sign) - sign;
            v += 0x80;
        }
        memset(b->cur_dec, v, (size_t)t);
        b->cur_dec += t;
    } else {
        while (b->cur_dec < dec_end) {
            if (bink_bits_left(bits) < 2)
                return -1;
            high = bink_tree_decode(bits, &col_high[*col_lastval]);
            if (high < 0)
                return -1;
            *col_lastval = high;
            v = bink_tree_decode(bits, &b->tree);
            if (v < 0)
                return -1;
            v = (high << 4) | v;
            if (version < 'i') {
                int sign = ((signed char)v) >> 7;

                v = ((v & 0x7F) ^ sign) - sign;
                v += 0x80;
            }
            *b->cur_dec++ = (unsigned char)v;
        }
    }
    return 0;
}

int bink_bundle_read_dcs(BinkBits *bits, BinkBundle *b,
                         int start_bits, int has_sign)
{
    int i;
    int j;
    int len;
    int len2;
    int bsize;
    int sign;
    int v;
    int v2;
    int r;
    int16_t *dst = (int16_t *)(void *)b->cur_dec;
    int16_t *dst_end = (int16_t *)(void *)b->data_end;

    r = bundle_count(bits, b, &len);
    if (r <= 0)
        return r;
    if (bink_bits_left(bits) < start_bits - has_sign)
        return -1;
    v = bink_bits_read(bits, start_bits - has_sign);
    if (v < 0)
        return -1;
    if (v && has_sign) {
        if (read_sign(bits, &sign) != 0)
            return -1;
        v = (v ^ sign) - sign;
    }
    if (dst_end - dst < 1)
        return -1;
    *dst++ = (int16_t)v;
    len--;
    for (i = 0; i < len; i += 8) {
        len2 = (len - i < 8) ? len - i : 8;
        if (dst_end - dst < len2)
            return -1;
        bsize = bink_bits_read(bits, 4);
        if (bsize < 0)
            return -1;
        if (bsize) {
            for (j = 0; j < len2; j++) {
                v2 = bink_bits_read(bits, bsize);
                if (v2 < 0)
                    return -1;
                if (v2) {
                    if (read_sign(bits, &sign) != 0)
                        return -1;
                    v2 = (v2 ^ sign) - sign;
                }
                v += v2;
                if (v < -32768 || v > 32767)
                    return -1;
                *dst++ = (int16_t)v;
            }
        } else {
            for (j = 0; j < len2; j++)
                *dst++ = (int16_t)v;
        }
    }

    b->cur_dec = (unsigned char *)dst;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Video context                                                        */
/* ------------------------------------------------------------------ */

#define DC_START_BITS 11
#define BINK_VALUE_ERROR INT_MIN

enum {
    SKIP_BLOCK = 0,
    SCALED_BLOCK,
    MOTION_BLOCK,
    RUN_BLOCK,
    RESIDUE_BLOCK,
    INTRA_BLOCK,
    FILL_BLOCK,
    INTER_BLOCK,
    PATTERN_BLOCK,
    RAW_BLOCK
};

static int align32(unsigned long value)
{
    return (int)((value + 31) & ~31UL);
}

static int int_log2(unsigned int value)
{
    int result = 0;

    while (value > 1) {
        value >>= 1;
        result++;
    }
    return result;
}

int bink_video_init(BinkVideo *v, unsigned long width, unsigned long height,
                    unsigned long flags, int version)
{
    int i;

    if (v == NULL || width == 0 || height == 0)
        return -1;

    memset(v, 0, sizeof(*v));
    v->width = width;
    v->height = height;
    v->version = version;
    v->has_alpha = (flags & BINK_FLAG_ALPHA) ? 1 : 0;
    v->swap_planes = (version >= 'h');

    v->bw = (int)((width + 7) >> 3);
    v->bh = (int)((height + 7) >> 3);
    v->cbw = (int)((width + 15) >> 4);
    v->cbh = (int)((height + 15) >> 4);

    v->stride[BINK_PLANE_Y] = align32(width);
    v->stride[BINK_PLANE_U] = align32((width + 1) >> 1);
    v->stride[BINK_PLANE_V] = align32((width + 1) >> 1);
    v->stride[BINK_PLANE_A] = align32(width);

    v->plane_width[BINK_PLANE_Y] = (int)width;
    v->plane_width[BINK_PLANE_U] = (int)((width + 1) >> 1);
    v->plane_width[BINK_PLANE_V] = (int)((width + 1) >> 1);
    v->plane_width[BINK_PLANE_A] = (int)width;

    v->plane_height[BINK_PLANE_Y] = (int)height;
    v->plane_height[BINK_PLANE_U] = (int)((height + 1) >> 1);
    v->plane_height[BINK_PLANE_V] = (int)((height + 1) >> 1);
    v->plane_height[BINK_PLANE_A] = (int)height;

    v->alloc_height[BINK_PLANE_Y] = v->bh * 8;
    v->alloc_height[BINK_PLANE_U] = v->cbh * 8;
    v->alloc_height[BINK_PLANE_V] = v->cbh * 8;
    v->alloc_height[BINK_PLANE_A] = v->bh * 8;

    for (i = 0; i < 4; i++) {
        size_t bytes;

        if (i == BINK_PLANE_A && !v->has_alpha)
            continue;
        bytes = (size_t)v->stride[i] * (size_t)v->alloc_height[i];
        v->cur[i] = (unsigned char *)calloc(1, bytes);
        v->ref[i] = (unsigned char *)calloc(1, bytes);
        if (v->cur[i] == NULL || v->ref[i] == NULL) {
            bink_video_uninit(v);
            return -1;
        }
    }

    v->bundle_data = (unsigned char *)calloc(
        (size_t)v->bw * (size_t)v->bh * 64, BINK_NB_SRC);
    if (v->bundle_data == NULL) {
        bink_video_uninit(v);
        return -1;
    }
    {
        size_t bundle_size = (size_t)v->bw * (size_t)v->bh * 64;

        for (i = 0; i < BINK_NB_SRC; i++) {
            v->bundles[i].data = v->bundle_data + bundle_size * (size_t)i;
            v->bundles[i].data_end = v->bundles[i].data + bundle_size;
        }
    }

    return 0;
}

void bink_video_uninit(BinkVideo *v)
{
    int i;

    if (v == NULL)
        return;
    for (i = 0; i < 4; i++) {
        free(v->cur[i]);
        free(v->ref[i]);
        v->cur[i] = NULL;
        v->ref[i] = NULL;
    }
    free(v->bundle_data);
    v->bundle_data = NULL;
    free(v->rgb);
    v->rgb = NULL;
    v->rgb_size = 0;
    v->rgb_valid = 0;
}

void bink_video_reset(BinkVideo *v)
{
    int i;

    if (v == NULL)
        return;
    for (i = 0; i < 4; i++) {
        size_t bytes;

        if (v->cur[i] == NULL || v->ref[i] == NULL)
            continue;
        bytes = (size_t)v->stride[i] * (size_t)v->alloc_height[i];
        memset(v->cur[i], 0, bytes);
        memset(v->ref[i], 0, bytes);
    }
    v->has_prev = 0;
    v->rgb_valid = 0;
}

unsigned char *bink_video_plane(BinkVideo *v, int plane)
{
    if (v == NULL || plane < 0 || plane > 3)
        return NULL;
    if (!v->has_prev)
        return NULL;
    return v->cur[plane];
}

int bink_video_stride(const BinkVideo *v, int plane)
{
    if (v == NULL || plane < 0 || plane > 3)
        return 0;
    return v->stride[plane];
}

int bink_video_plane_width(const BinkVideo *v, int plane)
{
    if (v == NULL || plane < 0 || plane > 3)
        return 0;
    return v->plane_width[plane];
}

int bink_video_plane_height(const BinkVideo *v, int plane)
{
    if (v == NULL || plane < 0 || plane > 3)
        return 0;
    return v->plane_height[plane];
}

const unsigned char *bink_video_rgb(BinkVideo *v)
{
    (void)v;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Bundle header handling                                               */
/* ------------------------------------------------------------------ */

static void init_lengths(BinkVideo *v, int width, int bw)
{
    int w = (width + 7) & ~7;

    v->bundles[BINK_SRC_BLOCK_TYPES].len = int_log2((unsigned)((w >> 3) + 511)) + 1;
    v->bundles[BINK_SRC_SUB_BLOCK_TYPES].len = int_log2((unsigned)((w >> 4) + 511)) + 1;
    v->bundles[BINK_SRC_COLORS].len = int_log2((unsigned)(bw * 64 + 511)) + 1;
    v->bundles[BINK_SRC_INTRA_DC].len =
    v->bundles[BINK_SRC_INTER_DC].len =
    v->bundles[BINK_SRC_X_OFF].len =
    v->bundles[BINK_SRC_Y_OFF].len = int_log2((unsigned)((w >> 3) + 511)) + 1;
    v->bundles[BINK_SRC_PATTERN].len = int_log2((unsigned)((bw << 3) + 511)) + 1;
    v->bundles[BINK_SRC_RUN].len = int_log2((unsigned)(bw * 48 + 511)) + 1;
}

static int read_bundle_header(BinkVideo *v, BinkBits *bits, int num)
{
    int i;
    int ret;

    if (num == BINK_SRC_COLORS) {
        for (i = 0; i < 16; i++) {
            ret = bink_tree_read(bits, &v->col_high[i]);
            if (ret < 0)
                return -1;
        }
        v->col_lastval = 0;
    }
    if (num != BINK_SRC_INTRA_DC && num != BINK_SRC_INTER_DC) {
        ret = bink_tree_read(bits, &v->bundles[num].tree);
        if (ret < 0)
            return -1;
    }
    v->bundles[num].cur_dec = v->bundles[num].data;
    v->bundles[num].cur_ptr = v->bundles[num].data;
    return 0;
}

static int get_value(BinkVideo *v, int bundle)
{
    BinkBundle *b = &v->bundles[bundle];

    if (bundle < BINK_SRC_X_OFF || bundle == BINK_SRC_RUN) {
        if (b->cur_ptr == NULL || b->cur_ptr >= b->data_end)
            return BINK_VALUE_ERROR;
        return *b->cur_ptr++;
    }
    if (bundle == BINK_SRC_X_OFF || bundle == BINK_SRC_Y_OFF) {
        if (b->cur_ptr == NULL || b->cur_ptr >= b->data_end)
            return BINK_VALUE_ERROR;
        return (int)(int8_t)*b->cur_ptr++;
    }
    if (b->cur_ptr == NULL || b->data_end - b->cur_ptr < 2)
        return BINK_VALUE_ERROR;
    {
        int value = (int)b->cur_ptr[0] | ((int)b->cur_ptr[1] << 8);

        b->cur_ptr += 2;
        return (int)(int16_t)value;
    }
}

/* ------------------------------------------------------------------ */
/* DCT coefficient and residue decoding                                 */
/* ------------------------------------------------------------------ */

static int read_dct_coeffs(BinkBits *bits, int32_t block[64],
                           const uint8_t *scan, int *coef_count_,
                           int coef_idx[64], int q)
{
    int coef_list[128];
    int mode_list[128];
    int i;
    int t;
    int cbits;
    int ccoef;
    int mode;
    int sign;
    int list_start = 64;
    int list_end = 64;
    int list_pos;
    int coef_count = 0;
    int quant_idx;

    if (bink_bits_left(bits) < 4)
        return -1;

    coef_list[list_end] = 4;  mode_list[list_end++] = 0;
    coef_list[list_end] = 24; mode_list[list_end++] = 0;
    coef_list[list_end] = 44; mode_list[list_end++] = 0;
    coef_list[list_end] = 1;  mode_list[list_end++] = 3;
    coef_list[list_end] = 2;  mode_list[list_end++] = 3;
    coef_list[list_end] = 3;  mode_list[list_end++] = 3;

    cbits = bink_bits_read(bits, 4);
    if (cbits < 0)
        return -1;
    for (cbits--; cbits >= 0; cbits--) {
        list_pos = list_start;
        while (list_pos < list_end) {
            int bit;

            if (!(mode_list[list_pos] | coef_list[list_pos])) {
                list_pos++;
                continue;
            }
            bit = bink_bits_read1(bits);
            if (bit < 0)
                return -1;
            if (!bit) {
                list_pos++;
                continue;
            }
            ccoef = coef_list[list_pos];
            mode = mode_list[list_pos];
            switch (mode) {
            case 0:
                coef_list[list_pos] = ccoef + 4;
                mode_list[list_pos] = 1;
                /* fall through */
            case 2:
                if (mode == 2) {
                    coef_list[list_pos] = 0;
                    mode_list[list_pos++] = 0;
                }
                for (i = 0; i < 4; i++, ccoef++) {
                    int cbit = bink_bits_read1(bits);

                    if (cbit < 0)
                        return -1;
                    if (cbit) {
                        if (list_start <= 0)
                            return -1;
                        coef_list[--list_start] = ccoef;
                        mode_list[list_start] = 3;
                    } else {
                        if (!cbits) {
                            int sbit = bink_bits_read1(bits);

                            if (sbit < 0)
                                return -1;
                            t = 1 - (sbit << 1);
                        } else {
                            int v = bink_bits_read(bits, cbits);
                            int sbit = bink_bits_read1(bits);

                            if (v < 0 || sbit < 0)
                                return -1;
                            t = v | (1 << cbits);
                            sign = -sbit;
                            t = (t ^ sign) - sign;
                        }
                        if (ccoef < 0 || ccoef >= 64 || coef_count >= 64)
                            return -1;
                        block[scan[ccoef]] = t;
                        coef_idx[coef_count++] = ccoef;
                    }
                }
                break;
            case 1:
                mode_list[list_pos] = 2;
                for (i = 0; i < 3; i++) {
                    ccoef += 4;
                    if (list_end >= 128)
                        return -1;
                    coef_list[list_end] = ccoef;
                    mode_list[list_end++] = 2;
                }
                break;
            case 3:
                if (!cbits) {
                    int sbit = bink_bits_read1(bits);

                    if (sbit < 0)
                        return -1;
                    t = 1 - (sbit << 1);
                } else {
                    int v = bink_bits_read(bits, cbits);
                    int sbit = bink_bits_read1(bits);

                    if (v < 0 || sbit < 0)
                        return -1;
                    t = v | (1 << cbits);
                    sign = -sbit;
                    t = (t ^ sign) - sign;
                }
                if (ccoef < 0 || ccoef >= 64 || coef_count >= 64)
                    return -1;
                block[scan[ccoef]] = t;
                coef_idx[coef_count++] = ccoef;
                coef_list[list_pos] = 0;
                mode_list[list_pos++] = 0;
                break;
            default:
                return -1;
            }
        }
    }

    if (q == -1) {
        quant_idx = bink_bits_read(bits, 4);
        if (quant_idx < 0)
            return -1;
    } else {
        quant_idx = q;
        if (quant_idx > 15 || quant_idx < 0)
            return -1;
    }

    *coef_count_ = coef_count;
    return quant_idx;
}

static void unquantize_dct_coeffs(int32_t block[64], const int32_t quant[64],
                                  int coef_count, const int coef_idx[64])
{
    int i;

    block[0] = (int)(block[0] * quant[0]) >> 11;
    for (i = 0; i < coef_count; i++) {
        int idx = coef_idx[i];

        block[bink_scan[idx]] =
            (int)(block[bink_scan[idx]] * quant[idx]) >> 11;
    }
}

static int read_residue(BinkBits *bits, int16_t block[64], int masks_count)
{
    int coef_list[128];
    int mode_list[128];
    int i;
    int sign;
    int mask;
    int ccoef;
    int mode;
    int list_start = 64;
    int list_end = 64;
    int list_pos;
    int nz_coeff[64];
    int nz_coeff_count = 0;
    int mask_bits;

    coef_list[list_end] = 4;  mode_list[list_end++] = 0;
    coef_list[list_end] = 24; mode_list[list_end++] = 0;
    coef_list[list_end] = 44; mode_list[list_end++] = 0;
    coef_list[list_end] = 0;  mode_list[list_end++] = 2;

    mask_bits = bink_bits_read(bits, 3);
    if (mask_bits < 0)
        return -1;

    for (mask = 1 << mask_bits; mask; mask >>= 1) {
        for (i = 0; i < nz_coeff_count; i++) {
            int bit = bink_bits_read1(bits);

            if (bit < 0)
                return -1;
            if (!bit)
                continue;
            if (block[nz_coeff[i]] < 0)
                block[nz_coeff[i]] -= (int16_t)mask;
            else
                block[nz_coeff[i]] += (int16_t)mask;
            masks_count--;
            if (masks_count < 0)
                return 0;
        }
        list_pos = list_start;
        while (list_pos < list_end) {
            int bit;

            if (!(coef_list[list_pos] | mode_list[list_pos])) {
                list_pos++;
                continue;
            }
            bit = bink_bits_read1(bits);
            if (bit < 0)
                return -1;
            if (!bit) {
                list_pos++;
                continue;
            }
            ccoef = coef_list[list_pos];
            mode = mode_list[list_pos];
            switch (mode) {
            case 0:
                coef_list[list_pos] = ccoef + 4;
                mode_list[list_pos] = 1;
                /* fall through */
            case 2:
                if (mode == 2) {
                    coef_list[list_pos] = 0;
                    mode_list[list_pos++] = 0;
                }
                for (i = 0; i < 4; i++, ccoef++) {
                    int cbit = bink_bits_read1(bits);

                    if (cbit < 0)
                        return -1;
                    if (cbit) {
                        if (list_start <= 0)
                            return -1;
                        coef_list[--list_start] = ccoef;
                        mode_list[list_start] = 3;
                    } else {
                        int sbit = bink_bits_read1(bits);

                        if (sbit < 0)
                            return -1;
                        if (ccoef < 0 || ccoef >= 64 ||
                            nz_coeff_count >= 64)
                            return -1;
                        nz_coeff[nz_coeff_count++] = bink_scan[ccoef];
                        sign = -sbit;
                        block[bink_scan[ccoef]] =
                            (int16_t)((mask ^ sign) - sign);
                        masks_count--;
                        if (masks_count < 0)
                            return 0;
                    }
                }
                break;
            case 1:
                mode_list[list_pos] = 2;
                for (i = 0; i < 3; i++) {
                    ccoef += 4;
                    if (list_end >= 128)
                        return -1;
                    coef_list[list_end] = ccoef;
                    mode_list[list_end++] = 2;
                }
                break;
            case 3:
            {
                int sbit = bink_bits_read1(bits);

                if (sbit < 0)
                    return -1;
                if (ccoef < 0 || ccoef >= 64 || nz_coeff_count >= 64)
                    return -1;
                nz_coeff[nz_coeff_count++] = bink_scan[ccoef];
                sign = -sbit;
                block[bink_scan[ccoef]] = (int16_t)((mask ^ sign) - sign);
                coef_list[list_pos] = 0;
                mode_list[list_pos++] = 0;
                masks_count--;
                if (masks_count < 0)
                    return 0;
                break;
            }
            default:
                return -1;
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Plane decoding                                                       */
/* ------------------------------------------------------------------ */

static void copy_block_overlapped(unsigned char *dst, const unsigned char *src,
                                  int stride)
{
    unsigned char tmp[64];
    int i;

    for (i = 0; i < 8; i++)
        memcpy(tmp + i * 8, src + (size_t)i * (size_t)stride, 8);
    for (i = 0; i < 8; i++)
        memcpy(dst + (size_t)i * (size_t)stride, tmp + i * 8, 8);
}

static int bink_put_pixels(BinkVideo *v, unsigned char *dst,
                           unsigned char *prev, int stride,
                           unsigned char *ref_start, unsigned char *ref_end)
{
    int xoff = get_value(v, BINK_SRC_X_OFF);
    int yoff = get_value(v, BINK_SRC_Y_OFF);
    unsigned char *ref;

    if (xoff == BINK_VALUE_ERROR || yoff == BINK_VALUE_ERROR)
        return -1;
    ref = prev + xoff + (size_t)yoff * (size_t)stride;
    if (ref < ref_start || ref > ref_end)
        return 0; /* invalid stream: leave the destination block unchanged */
    copy_block_overlapped(dst, ref, stride);
    return 0;
}

static int decode_plane(BinkVideo *v, BinkBits *bits, int plane, int is_chroma)
{
    int i;
    int j;
    int bx;
    int by;
    int blk;
    int ret;
    int quant_idx;
    int coef_count;
    int coef_idx[64];
    int coordmap[64];
    int col[2];
    int stride = v->stride[plane];
    int bw = is_chroma ? v->cbw : v->bw;
    int bh = is_chroma ? v->cbh : v->bh;
    int width = v->plane_width[plane];
    int height = v->plane_height[plane];
    unsigned char *frame_data = v->cur[plane];
    unsigned char *last_data = v->has_prev ? v->ref[plane] : NULL;
    unsigned char *dst;
    unsigned char *prev;
    unsigned char *ref_start;
    unsigned char *ref_end;
    unsigned char ublock[64];
    int32_t dctblock[64];
    int16_t block[64];

    (void)height;

    init_lengths(v, width > 8 ? width : 8, bw);
    for (i = 0; i < BINK_NB_SRC; i++) {
        ret = read_bundle_header(v, bits, i);
        if (ret < 0)
            return -1;
    }

    ref_start = last_data ? last_data : frame_data;
    ref_end = ref_start + (size_t)(bw - 1 + stride * (bh - 1)) * 8;

    for (i = 0; i < 64; i++)
        coordmap[i] = (i & 7) + (i >> 3) * stride;

    for (by = 0; by < bh; by++) {
        ret = bink_bundle_read_block_types(bits, &v->bundles[BINK_SRC_BLOCK_TYPES], v->version);
        if (ret < 0)
            return -1;
        ret = bink_bundle_read_block_types(bits, &v->bundles[BINK_SRC_SUB_BLOCK_TYPES], v->version);
        if (ret < 0)
            return -1;
        ret = bink_bundle_read_colors(bits, &v->bundles[BINK_SRC_COLORS], v->col_high, &v->col_lastval, v->version);
        if (ret < 0)
            return -1;
        ret = bink_bundle_read_patterns(bits, &v->bundles[BINK_SRC_PATTERN]);
        if (ret < 0)
            return -1;
        ret = bink_bundle_read_motion_values(bits, &v->bundles[BINK_SRC_X_OFF]);
        if (ret < 0)
            return -1;
        ret = bink_bundle_read_motion_values(bits, &v->bundles[BINK_SRC_Y_OFF]);
        if (ret < 0)
            return -1;
        ret = bink_bundle_read_dcs(bits, &v->bundles[BINK_SRC_INTRA_DC], DC_START_BITS, 0);
        if (ret < 0)
            return -1;
        ret = bink_bundle_read_dcs(bits, &v->bundles[BINK_SRC_INTER_DC], DC_START_BITS, 1);
        if (ret < 0)
            return -1;
        ret = bink_bundle_read_runs(bits, &v->bundles[BINK_SRC_RUN]);
        if (ret < 0)
            return -1;

        dst = frame_data + (size_t)8 * (size_t)by * (size_t)stride;
        prev = (last_data ? last_data : frame_data) +
               (size_t)8 * (size_t)by * (size_t)stride;
        for (bx = 0; bx < bw; bx++, dst += 8, prev += 8) {
            blk = get_value(v, BINK_SRC_BLOCK_TYPES);
            if (blk == BINK_VALUE_ERROR)
                return -1;
            if (((by & 1) || (bx & 1)) && blk == SCALED_BLOCK) {
                bx++;
                dst += 8;
                prev += 8;
                continue;
            }
            switch (blk) {
            case SKIP_BLOCK:
                copy_block_overlapped(dst, prev, stride);
                break;
            case SCALED_BLOCK:
                blk = get_value(v, BINK_SRC_SUB_BLOCK_TYPES);
                if (blk == BINK_VALUE_ERROR)
                    return -1;
                switch (blk) {
                case RUN_BLOCK:
                {
                    const unsigned char *scan;
                    int pattern = bink_bits_read(bits, 4);

                    if (pattern < 0)
                        return -1;
                    scan = bink_patterns[pattern];
                    i = 0;
                    do {
                        int run = get_value(v, BINK_SRC_RUN);
                        int bit;

                        if (run == BINK_VALUE_ERROR)
                            return -1;
                        run += 1;
                        i += run;
                        if (i > 64)
                            return -1;
                        bit = bink_bits_read1(bits);
                        if (bit < 0)
                            return -1;
                        if (bit) {
                            int value = get_value(v, BINK_SRC_COLORS);

                            if (value == BINK_VALUE_ERROR)
                                return -1;
                            for (j = 0; j < run; j++)
                                ublock[*scan++] = (unsigned char)value;
                        } else {
                            for (j = 0; j < run; j++) {
                                int value = get_value(v, BINK_SRC_COLORS);

                                if (value == BINK_VALUE_ERROR)
                                    return -1;
                                ublock[*scan++] = (unsigned char)value;
                            }
                        }
                    } while (i < 63);
                    if (i == 63) {
                        int value = get_value(v, BINK_SRC_COLORS);

                        if (value == BINK_VALUE_ERROR)
                            return -1;
                        ublock[*scan] = (unsigned char)value;
                    }
                    break;
                }
                case INTRA_BLOCK:
                {
                    int dc = get_value(v, BINK_SRC_INTRA_DC);

                    if (dc == BINK_VALUE_ERROR)
                        return -1;
                    memset(dctblock, 0, sizeof(dctblock));
                    dctblock[0] = dc;
                    quant_idx = read_dct_coeffs(bits, dctblock, bink_scan,
                                                &coef_count, coef_idx, -1);
                    if (quant_idx < 0)
                        return -1;
                    unquantize_dct_coeffs(dctblock,
                                          bink_intra_quant[quant_idx],
                                          coef_count, coef_idx);
                    bink_idct_put(ublock, 8, dctblock);
                    break;
                }
                case FILL_BLOCK:
                {
                    int value = get_value(v, BINK_SRC_COLORS);

                    if (value == BINK_VALUE_ERROR)
                        return -1;
                    bink_fill_block(dst, stride, 16, (unsigned char)value);
                    break;
                }
                case PATTERN_BLOCK:
                    for (i = 0; i < 2; i++) {
                        col[i] = get_value(v, BINK_SRC_COLORS);
                        if (col[i] == BINK_VALUE_ERROR)
                            return -1;
                    }
                    for (j = 0; j < 8; j++) {
                        int value = get_value(v, BINK_SRC_PATTERN);

                        if (value == BINK_VALUE_ERROR)
                            return -1;
                        for (i = 0; i < 8; i++, value >>= 1)
                            ublock[i + j * 8] = (unsigned char)col[value & 1];
                    }
                    break;
                case RAW_BLOCK:
                    for (j = 0; j < 8; j++)
                        for (i = 0; i < 8; i++) {
                            int value = get_value(v, BINK_SRC_COLORS);

                            if (value == BINK_VALUE_ERROR)
                                return -1;
                            ublock[i + j * 8] = (unsigned char)value;
                        }
                    break;
                default:
                    return -1;
                }
                if (blk != FILL_BLOCK)
                    bink_scale_block(ublock, dst, stride);
                bx++;
                dst += 8;
                prev += 8;
                break;
            case MOTION_BLOCK:
                ret = bink_put_pixels(v, dst, prev, stride, ref_start,
                                      ref_end);
                if (ret < 0)
                    return -1;
                break;
            case RUN_BLOCK:
            {
                const unsigned char *scan;
                int pattern = bink_bits_read(bits, 4);

                if (pattern < 0)
                    return -1;
                scan = bink_patterns[pattern];
                i = 0;
                do {
                    int run = get_value(v, BINK_SRC_RUN);
                    int bit;

                    if (run == BINK_VALUE_ERROR)
                        return -1;
                    run += 1;
                    i += run;
                    if (i > 64)
                        return -1;
                    bit = bink_bits_read1(bits);
                    if (bit < 0)
                        return -1;
                    if (bit) {
                        int value = get_value(v, BINK_SRC_COLORS);

                        if (value == BINK_VALUE_ERROR)
                            return -1;
                        for (j = 0; j < run; j++)
                            dst[coordmap[*scan++]] = (unsigned char)value;
                    } else {
                        for (j = 0; j < run; j++) {
                            int value = get_value(v, BINK_SRC_COLORS);

                            if (value == BINK_VALUE_ERROR)
                                return -1;
                            dst[coordmap[*scan++]] = (unsigned char)value;
                        }
                    }
                } while (i < 63);
                if (i == 63) {
                    int value = get_value(v, BINK_SRC_COLORS);

                    if (value == BINK_VALUE_ERROR)
                        return -1;
                    dst[coordmap[*scan]] = (unsigned char)value;
                }
                break;
            }
            case RESIDUE_BLOCK:
            {
                int masks;

                ret = bink_put_pixels(v, dst, prev, stride, ref_start,
                                      ref_end);
                if (ret < 0)
                    return -1;
                memset(block, 0, sizeof(block));
                masks = bink_bits_read(bits, 7);
                if (masks < 0)
                    return -1;
                ret = read_residue(bits, block, masks);
                if (ret < 0)
                    return -1;
                bink_add_pixels8(dst, block, stride);
                break;
            }
            case INTRA_BLOCK:
            {
                int dc = get_value(v, BINK_SRC_INTRA_DC);

                if (dc == BINK_VALUE_ERROR)
                    return -1;
                memset(dctblock, 0, sizeof(dctblock));
                dctblock[0] = dc;
                quant_idx = read_dct_coeffs(bits, dctblock, bink_scan,
                                            &coef_count, coef_idx, -1);
                if (quant_idx < 0)
                    return -1;
                unquantize_dct_coeffs(dctblock, bink_intra_quant[quant_idx],
                                      coef_count, coef_idx);
                bink_idct_put(dst, stride, dctblock);
                break;
            }
            case FILL_BLOCK:
            {
                int value = get_value(v, BINK_SRC_COLORS);

                if (value == BINK_VALUE_ERROR)
                    return -1;
                bink_fill_block(dst, stride, 8, (unsigned char)value);
                break;
            }
            case INTER_BLOCK:
            {
                int dc;

                ret = bink_put_pixels(v, dst, prev, stride, ref_start,
                                      ref_end);
                if (ret < 0)
                    return -1;
                dc = get_value(v, BINK_SRC_INTER_DC);
                if (dc == BINK_VALUE_ERROR)
                    return -1;
                memset(dctblock, 0, sizeof(dctblock));
                dctblock[0] = dc;
                quant_idx = read_dct_coeffs(bits, dctblock, bink_scan,
                                            &coef_count, coef_idx, -1);
                if (quant_idx < 0)
                    return -1;
                unquantize_dct_coeffs(dctblock, bink_inter_quant[quant_idx],
                                      coef_count, coef_idx);
                bink_idct_add(dst, stride, dctblock);
                break;
            }
            case PATTERN_BLOCK:
                for (i = 0; i < 2; i++) {
                    col[i] = get_value(v, BINK_SRC_COLORS);
                    if (col[i] == BINK_VALUE_ERROR)
                        return -1;
                }
                for (i = 0; i < 8; i++) {
                    int value = get_value(v, BINK_SRC_PATTERN);

                    if (value == BINK_VALUE_ERROR)
                        return -1;
                    for (j = 0; j < 8; j++, value >>= 1)
                        dst[i * stride + j] = (unsigned char)col[value & 1];
                }
                break;
            case RAW_BLOCK:
                if (v->bundles[BINK_SRC_COLORS].data_end -
                        v->bundles[BINK_SRC_COLORS].cur_ptr < 64)
                    return -1;
                for (i = 0; i < 8; i++)
                    memcpy(dst + (size_t)i * (size_t)stride,
                           v->bundles[BINK_SRC_COLORS].cur_ptr + i * 8, 8);
                v->bundles[BINK_SRC_COLORS].cur_ptr += 64;
                break;
            default:
                return -1;
            }
        }
    }

    if (bits->bitpos & 0x1F)
        bink_bits_skip(bits, 32 - (int)(bits->bitpos & 0x1F));

    return 0;
}

int bink_video_decode(BinkVideo *v, const unsigned char *data, size_t size)
{
    BinkBits bits;
    int plane;
    int ret;
    int i;

    if (v == NULL || data == NULL)
        return -1;

    bink_bits_init(&bits, data, size);

    /* Ping-pong: decode into the buffer that is not the current reference.
     * On the first frame there is no reference, so the decoder reads back
     * from the buffer it is writing (matching FFmpeg's first-frame path). */
    if (v->has_prev) {
        for (i = 0; i < 4; i++) {
            unsigned char *swap = v->cur[i];

            v->cur[i] = v->ref[i];
            v->ref[i] = swap;
        }
    }

    if (v->has_alpha) {
        if (v->version >= 'i')
            bink_bits_skip(&bits, 32);
        ret = decode_plane(v, &bits, BINK_PLANE_A, 0);
        if (ret < 0)
            return -1;
    }
    if (v->version >= 'i')
        bink_bits_skip(&bits, 32);

    for (plane = 0; plane < 3; plane++) {
        int plane_idx = (!plane || !v->swap_planes) ? plane : (plane ^ 3);

        ret = decode_plane(v, &bits, plane_idx, !!plane);
        if (ret < 0)
            return -1;
        if (bink_bits_left(&bits) <= 0)
            break;
    }

    v->has_prev = 1;
    v->rgb_valid = 0;
    return 0;
}
