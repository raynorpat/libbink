/*
 * Bink 1 encoder DCT coefficient math and coefficient tree writer.
 *
 * Copyright (c) 2026 libbink contributors.
 * Licensed under the GNU Lesser General Public License, version 2.1 or later.
 *
 * Provenance: the forward transform inverts the ideal column transform of
 * the decoder's integer IDCT (IDCT_TRANSFORM in src/bink_dsp.c, ported from
 * FFmpeg libavcodec/bink.c). The coefficient tree writer mirrors
 * read_dct_coeffs in src/bink_video.c, including the root list, the mode
 * transitions, and the bit-plane value layout. The decoder is the authority.
 */

#include "bink_enc_dct.h"

#include "bink_video.h"

#include <math.h>
#include <string.h>

/* Decoder IDCT constants scaled by 2048. */
#define DCT_A1 (2896 / 2048.0)
#define DCT_A2 (2217 / 2048.0)
#define DCT_A3 (3784 / 2048.0)
#define DCT_A4 (-5352 / 2048.0)

const uint8_t *bink_enc_scan(void)
{
    return bink_video_scan();
}

const int32_t *bink_enc_intra_quant(int index)
{
    return bink_video_intra_quant(index);
}

const int32_t *bink_enc_inter_quant(int index)
{
    return bink_video_inter_quant(index);
}

/* ------------------------------------------------------------------ */
/* Forward transform                                                    */
/* ------------------------------------------------------------------ */

/* Ideal real-valued version of the decoder's 8-point column transform. */
static void dct_col(const double s[8], double out[8])
{
    double a0 = s[0] + s[4];
    double a1 = s[0] - s[4];
    double a2 = s[2] + s[6];
    double a3 = DCT_A1 * (s[2] - s[6]);
    double a4 = s[5] + s[3];
    double a5 = s[5] - s[3];
    double a6 = s[1] + s[7];
    double a7 = s[1] - s[7];
    double b0 = a4 + a6;
    double b1 = DCT_A3 * (a5 + a7);
    double b2 = DCT_A4 * a5 - b0 + b1;
    double b3 = DCT_A1 * (a6 - a4) - b2;
    double b4 = DCT_A2 * a7 + b3 - b1;

    out[0] = a0 + a2 + b0;
    out[1] = a1 + a3 - a2 + b2;
    out[2] = a1 - a3 + a2 + b3;
    out[3] = a0 - a2 - b4;
    out[4] = a0 - a2 + b4;
    out[5] = a1 - a3 + a2 - b3;
    out[6] = a1 + a3 - a2 - b2;
    out[7] = a0 + a2 - b0;
}

static int dct_invert(const double src[8][8], double inv[8][8])
{
    double a[8][16];
    int row;
    int col;
    int i;

    for (row = 0; row < 8; row++) {
        for (col = 0; col < 8; col++) {
            a[row][col] = src[row][col];
            a[row][8 + col] = (row == col) ? 1.0 : 0.0;
        }
    }
    for (col = 0; col < 8; col++) {
        int pivot = col;
        double scale;

        for (row = col + 1; row < 8; row++) {
            if (fabs(a[row][col]) > fabs(a[pivot][col]))
                pivot = row;
        }
        if (fabs(a[pivot][col]) < 1.0e-12)
            return -1;
        if (pivot != col) {
            for (i = 0; i < 16; i++) {
                double tmp = a[col][i];

                a[col][i] = a[pivot][i];
                a[pivot][i] = tmp;
            }
        }
        scale = 1.0 / a[col][col];
        for (i = 0; i < 16; i++)
            a[col][i] *= scale;
        for (row = 0; row < 8; row++) {
            double factor;

            if (row == col)
                continue;
            factor = a[row][col];
            if (factor == 0.0)
                continue;
            for (i = 0; i < 16; i++)
                a[row][i] -= factor * a[col][i];
        }
    }
    for (row = 0; row < 8; row++) {
        for (col = 0; col < 8; col++)
            inv[row][col] = a[row][8 + col];
    }
    return 0;
}

static double dct_ci[8][8];
/* Single-threaded encoder; see design doc. Built on first use. */
static int dct_ready;

static int dct_init(void)
{
    double c[8][8];
    int f;
    int s;

    if (dct_ready)
        return 0;
    for (f = 0; f < 8; f++) {
        double unit[8];
        double out[8];
        int i;

        for (i = 0; i < 8; i++)
            unit[i] = (i == f) ? 1.0 : 0.0;
        dct_col(unit, out);
        for (s = 0; s < 8; s++)
            c[s][f] = out[s];
    }
    if (dct_invert(c, dct_ci) != 0)
        return -1;
    dct_ready = 1;
    return 0;
}

void bink_enc_fdct(const uint8_t pixels[64], double coef[64])
{
    double m[8][8];
    double tmp[8][8];
    int row;
    int col;
    int k;

    if (dct_init() != 0) {
        memset(coef, 0, 64 * sizeof(coef[0]));
        return;
    }
    for (row = 0; row < 8; row++) {
        for (col = 0; col < 8; col++)
            m[row][col] = 256.0 * (double)pixels[row * 8 + col];
    }
    /* tmp = Ci * M */
    for (row = 0; row < 8; row++) {
        for (col = 0; col < 8; col++) {
            double sum = 0.0;

            for (k = 0; k < 8; k++)
                sum += dct_ci[row][k] * m[k][col];
            tmp[row][col] = sum;
        }
    }
    /* coef = tmp * Ci^T */
    for (row = 0; row < 8; row++) {
        for (col = 0; col < 8; col++) {
            double sum = 0.0;

            for (k = 0; k < 8; k++)
                sum += tmp[row][k] * dct_ci[col][k];
            coef[row * 8 + col] = sum;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Quantization                                                        */
/* ------------------------------------------------------------------ */

int bink_enc_quantize(const double coef[64], const int32_t quant[64],
                      int16_t t[64])
{
    const uint8_t *scan = bink_enc_scan();
    int i;

    for (i = 0; i < 64; i++) {
        double scaled = coef[scan[i]] * 2048.0 / (double)quant[i];
        long v;

        if (!(scaled > -32768.0 && scaled < 32768.0))
            return -1;
        v = lround(scaled);
        if (v <= -32768 || v >= 32768)
            return -1;
        t[i] = (int16_t)v;
    }
    return 0;
}

void bink_enc_unquantize(const int16_t t[64], const int32_t quant[64],
                         int32_t coef[64])
{
    const uint8_t *scan = bink_enc_scan();
    int i;

    for (i = 0; i < 64; i++)
        coef[scan[i]] = (int32_t)(((int64_t)t[i] * quant[i]) >> 11);
}

/* ------------------------------------------------------------------ */
/* Coefficient tree writer                                             */
/* ------------------------------------------------------------------ */

/* Bit plane of the largest magnitude bit, or -1 for zero. */
static int coeff_plane(int value)
{
    unsigned mag;
    int plane = -1;

    if (value == 0)
        return -1;
    mag = (unsigned)(value < 0 ? -value : value);
    while (mag != 0) {
        mag >>= 1;
        plane++;
    }
    return plane;
}

int bink_enc_cbits(const int16_t t[64])
{
    int cbits = 0;
    int i;

    for (i = 1; i < 64; i++) {
        int plane = coeff_plane(t[i]);

        if (plane >= cbits)
            cbits = plane + 1;
    }
    return cbits;
}

typedef struct {
    BinkBitWriter *bw;
    int count;
} CoeffBits;

static int coeff_put(CoeffBits *cb, unsigned value, int count)
{
    if (count < 0)
        return -1;
    if (cb->bw != NULL && bink_bw_put(cb->bw, value, count) != 0)
        return -1;
    cb->count += count;
    return 0;
}

/* Write one tree value at bit plane p: the remaining magnitude bits (none at
 * plane 0) and a sign bit, 1 meaning negative. */
static int put_value(CoeffBits *cb, int value, int plane)
{
    int magnitude = value < 0 ? -value : value;
    unsigned negative = value < 0 ? 1u : 0u;

    if (plane == 0)
        return coeff_put(cb, negative, 1);
    if (coeff_put(cb, (unsigned)(magnitude - (1 << plane)), plane) != 0)
        return -1;
    return coeff_put(cb, negative, 1);
}

/* The scan indices a group node will code or spawn: 20 for mode 0, 16 for
 * mode 1, and its own four for mode 2. */
static int group_has_plane(const int16_t t[64], int coef, int mode, int plane)
{
    int span = mode == 0 ? 20 : (mode == 1 ? 16 : 4);
    int end = coef + span;
    int i;

    if (end > 64)
        end = 64;
    for (i = coef; i < end; i++) {
        if (coeff_plane(t[i]) == plane)
            return 1;
    }
    return 0;
}

static int emit_tree(CoeffBits *cb, const int16_t t[64], int cbits)
{
    int coef_list[128];
    int mode_list[128];
    int list_start = 64;
    int list_end = 64;
    int list_pos;
    int plane;

    coef_list[list_end] = 4;  mode_list[list_end++] = 0;
    coef_list[list_end] = 24; mode_list[list_end++] = 0;
    coef_list[list_end] = 44; mode_list[list_end++] = 0;
    coef_list[list_end] = 1;  mode_list[list_end++] = 3;
    coef_list[list_end] = 2;  mode_list[list_end++] = 3;
    coef_list[list_end] = 3;  mode_list[list_end++] = 3;

    for (plane = cbits - 1; plane >= 0; plane--) {
        list_pos = list_start;
        while (list_pos < list_end) {
            int coef = coef_list[list_pos];
            int mode = mode_list[list_pos];
            int open;
            int i;

            if (!(mode | coef)) {
                list_pos++;
                continue;
            }
            if (mode == 3)
                open = coeff_plane(t[coef]) == plane;
            else
                open = group_has_plane(t, coef, mode, plane);
            if (coeff_put(cb, open ? 1u : 0u, 1) != 0)
                return -1;
            if (!open) {
                list_pos++;
                continue;
            }
            switch (mode) {
            case 0:
                coef_list[list_pos] = coef + 4;
                mode_list[list_pos] = 1;
                /* fall through */
            case 2:
                if (mode == 2) {
                    coef_list[list_pos] = 0;
                    mode_list[list_pos++] = 0;
                }
                for (i = 0; i < 4; i++, coef++) {
                    int tp = coeff_plane(t[coef]);

                    if (tp == plane) {
                        if (coeff_put(cb, 0, 1) != 0)
                            return -1;
                        if (put_value(cb, t[coef], plane) != 0)
                            return -1;
                    } else {
                        if (tp > plane)
                            return -1;
                        if (list_start <= 0)
                            return -1;
                        if (coeff_put(cb, 1, 1) != 0)
                            return -1;
                        coef_list[--list_start] = coef;
                        mode_list[list_start] = 3;
                    }
                }
                break;
            case 1:
                mode_list[list_pos] = 2;
                for (i = 0; i < 3; i++) {
                    coef += 4;
                    if (list_end >= 128)
                        return -1;
                    coef_list[list_end] = coef;
                    mode_list[list_end++] = 2;
                }
                break;
            case 3:
                if (put_value(cb, t[coef], plane) != 0)
                    return -1;
                coef_list[list_pos] = 0;
                mode_list[list_pos++] = 0;
                break;
            default:
                return -1;
            }
        }
    }
    return 0;
}

int bink_enc_emit_coeffs(BinkBitWriter *bw, const int16_t t[64], int cbits,
                         int quant_index)
{
    CoeffBits cb;

    if (cbits != bink_enc_cbits(t))
        return -1;
    if (cbits < 0 || cbits > 15 || quant_index < 0 || quant_index > 15)
        return -1;
    cb.bw = bw;
    cb.count = 0;
    if (coeff_put(&cb, (unsigned)cbits, 4) != 0)
        return -1;
    if (emit_tree(&cb, t, cbits) != 0)
        return -1;
    if (coeff_put(&cb, (unsigned)quant_index, 4) != 0)
        return -1;
    return cb.count;
}
