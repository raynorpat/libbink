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

/* Inverse of the ideal real-valued decoder column transform. That transform
 * is `dct_col` over the decoder's fixed-point IDCT constants scaled by 2048;
 * `dct_ci` is its 8x8 inverse, taken once by Gauss-Jordan elimination and
 * frozen here so the transform needs no lazy initialization (and no lock).
 * test_dct_roundtrip pins the values end to end: if any digit were wrong,
 * blocks would not reconstruct. */
static const double dct_ci[8][8] = {
    {0.12499999999999996, 0.12500000000000003, 0.12499999999999999, 0.125,
     0.12500000000000003, 0.12499999999999994, 0.125,
     0.12500000000000003},
    {0.24048886866918079, 0.2038866587244294, 0.13625364261161108,
     0.047855852556362453, -0.04785585255636244, -0.13625364261161102,
     -0.20388665872442943, -0.24048886866918076},
    {0.21339779005524856, 0.088397790055248657, -0.088397790055248615,
     -0.21339779005524859, -0.21339779005524864, -0.088397790055248587,
     0.088397790055248615, 0.21339779005524862},
    {0.17282533224223209, -0.040572457813016514, -0.20388665872442943,
     -0.11548886866918082, 0.11548886866918079, 0.20388665872442938,
     0.040572457813016549, -0.17282533224223212},
    {0.125, -0.125, -0.12500000000000003, 0.125, 0.125,
     -0.12499999999999999, -0.125, 0.12500000000000003},
    {0.07717466775776792, -0.13622312229748074, 0.027091078613932149,
     0.11548886866918076, -0.11548886866918077, -0.027091078613932177,
     0.13622312229748074, -0.077174667757767906},
    {0.036602209944751385, -0.088397790055248629, 0.088397790055248601,
     -0.036602209944751378, -0.036602209944751385, 0.088397790055248615,
     -0.088397790055248629, 0.036602209944751399},
    {0.0095111313308192252, -0.027091078613932167, 0.040541937498886176,
     -0.047855852556362426, 0.04785585255636244, -0.040541937498886176,
     0.027091078613932156, -0.0095111313308192235}
};

static void fdct_block(const int16_t pixels[64], double coef[64])
{
    double m[8][8];
    double tmp[8][8];
    int row;
    int col;
    int k;

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

void bink_enc_fdct(const uint8_t pixels[64], double coef[64])
{
    int16_t block[64];
    int i;

    for (i = 0; i < 64; i++)
        block[i] = (int16_t)pixels[i];
    fdct_block(block, coef);
}

void bink_enc_fdct_signed(const int16_t pixels[64], double coef[64])
{
    fdct_block(pixels, coef);
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
