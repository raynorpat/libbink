/**
 * Bink Audio decoder
 * Copyright (c) 2007-2011 Peter Ross (pross@xvid.org)
 * Copyright (c) 2009 Daniel Verkamp (daniel@drv.nu)
 *
 * This file is part of libbink, derived from FFmpeg's libavcodec/binkaudio.c.
 * It is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
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

#include "bink_audio.h"
#include "bink_bits.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Copied unchanged from FFmpeg libavcodec/wma_freqs.c. */
static const unsigned short bink_wma_critical_freqs[25] = {
      100,   200,  300,  400,  510,  630,   770,   920,
     1080,  1270, 1480, 1720, 2000, 2320,  2700,  3150,
     3700,  4400, 5300, 6400, 7700, 9500, 12000, 15500,
    24500
};

static const unsigned char bink_rle_length_tab[16] = {
    2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64
};

#define BINK_AUDIO_MAX_FRAME 4096

struct BinkAudio {
    unsigned long rate;
    int channels;
    int use_dct;
    unsigned long max_bytes;
    int first_block;
    int frame_len;
    int overlap_len;
    int num_bands;
    unsigned int bands[26];
    float root;
    float quant_table[96];
    float *previous;
    float *block;
    unsigned char *pcm;
    unsigned long pcm_bytes;
    double *fft_re;
    double *fft_im;
};

/* Decimation-in-time radix-2. inverse=0 is the forward sign. No 1/n. */
static void fft_transform(double *re, double *im, int n, int inverse)
{
    int i;
    int len;

    for (i = 1, len = 0; i < n; i++) {
        int bit = n >> 1;

        for (; len & bit; bit >>= 1)
            len ^= bit;
        len ^= bit;
        if (i < len) {
            double tr = re[i];
            double ti = im[i];

            re[i] = re[len];
            im[i] = im[len];
            re[len] = tr;
            im[len] = ti;
        }
    }

    for (len = 2; len <= n; len <<= 1) {
        double ang = (inverse ? 2.0 : -2.0) * M_PI / (double)len;
        double wlen_re = cos(ang);
        double wlen_im = sin(ang);
        int start;

        for (start = 0; start < n; start += len) {
            double w_re = 1.0;
            double w_im = 0.0;
            int half = len >> 1;
            int k;

            for (k = 0; k < half; k++) {
                int u = start + k;
                int v = u + half;
                double vr = re[v] * w_re - im[v] * w_im;
                double vi = re[v] * w_im + im[v] * w_re;
                double next_re = w_re * wlen_re - w_im * wlen_im;

                re[v] = re[u] - vr;
                im[v] = im[u] - vi;
                re[u] += vr;
                im[u] += vi;
                w_im = w_re * wlen_im + w_im * wlen_re;
                w_re = next_re;
            }
        }
    }
}

/*
 * Inverse real DFT, IDFT_C2R, matching FFmpeg 4.4 rdft_calc_c for the
 * transform DCT-III builds on. n is the number of real samples.
 */
static void rdft_idft_c2r(float *data, int n, double *re, double *im)
{
    int i;
    int n4 = n >> 2;
    int fft_n = n >> 1;
    const float k1 = 0.5f;
    const float k2 = -0.5f;
    float ev_re = data[0];

    data[0] = ev_re + data[1];
    data[1] = ev_re - data[1];

    for (i = 1; i < n4; i++) {
        int i1 = 2 * i;
        int i2 = n - i1;
        float od_im = k2 * (data[i2] - data[i1]);
        float ev_im = k1 * (data[i1 + 1] - data[i2 + 1]);
        float od_re = k2 * (data[i1 + 1] + data[i2 + 1]);
        double angle = 2.0 * M_PI * (double)i / (double)n;
        float tc = (float)cos(angle);
        /* ff_cos_tabs mirrors so tsin[i] = sin(2*pi*i/n), not -sin. */
        float ts = (float)sin(angle);
        float sum_re;
        float sum_im;

        ev_re = k1 * (data[i1] + data[i2]);
        sum_re = od_re * tc - od_im * ts;
        sum_im = od_im * tc + od_re * ts;
        data[i1] = ev_re + sum_re;
        data[i1 + 1] = ev_im + sum_im;
        data[i2] = ev_re - sum_re;
        data[i2 + 1] = sum_im - ev_im;
    }

    data[n / 2 + 1] = -data[n / 2 + 1];
    data[0] *= k1;
    data[1] *= k1;

    for (i = 0; i < fft_n; i++) {
        re[i] = (double)data[2 * i];
        im[i] = (double)data[2 * i + 1];
    }
    fft_transform(re, im, fft_n, 1);
    for (i = 0; i < fft_n; i++) {
        data[2 * i] = (float)re[i];
        data[2 * i + 1] = (float)im[i];
    }
}

/*
 * DFT_C2R, the transform FFmpeg 4.4 runs on a Bink RDFT block.
 * inverse unmangle with negative_sin, sign convention +1, forward FFT.
 */
static void rdft_dft_c2r(float *data, int n, double *re, double *im)
{
    int i;
    int n4 = n >> 2;
    int fft_n = n >> 1;
    const float k1 = 0.5f;
    const float k2 = -0.5f;
    float ev_re = data[0];

    data[0] = ev_re + data[1];
    data[1] = ev_re - data[1];

    for (i = 1; i < n4; i++) {
        int i1 = 2 * i;
        int i2 = n - i1;
        float od_im = k2 * (data[i2] - data[i1]);
        float ev_im = k1 * (data[i1 + 1] - data[i2 + 1]);
        float od_re = k2 * (data[i1 + 1] + data[i2 + 1]);
        double angle = 2.0 * M_PI * (double)i / (double)n;
        float tc = (float)cos(angle);
        float ts = (float)sin(angle);
        float sum_re;
        float sum_im;

        ev_re = k1 * (data[i1] + data[i2]);
        sum_re = od_re * tc + od_im * ts;
        sum_im = od_im * tc - od_re * ts;
        data[i1] = ev_re + sum_re;
        data[i1 + 1] = ev_im + sum_im;
        data[i2] = ev_re - sum_re;
        data[i2 + 1] = sum_im - ev_im;
    }

    data[0] *= k1;
    data[1] *= k1;

    for (i = 0; i < fft_n; i++) {
        re[i] = (double)data[2 * i];
        im[i] = (double)data[2 * i + 1];
    }
    fft_transform(re, im, fft_n, 0);
    for (i = 0; i < fft_n; i++) {
        data[2 * i] = (float)re[i];
        data[2 * i + 1] = (float)im[i];
    }
}

/* FFmpeg dct_calc_III. n is the full frame length. src is consumed. */
static void dct_iii(float *dst, float *src, int n, double *re, double *im)
{
    int i;
    float next = src[n - 1];
    float inv_n = 1.0f / (float)n;

    for (i = n - 2; i >= 2; i -= 2) {
        float val1 = src[i];
        float val2 = src[i - 1] - src[i + 1];
        double angle = M_PI * (double)i / (double)(2 * n);
        float c = (float)cos(angle);
        float s = (float)sin(angle);

        src[i] = c * val1 + s * val2;
        src[i + 1] = s * val1 - c * val2;
    }
    src[1] = 2.0f * next;

    rdft_idft_c2r(src, n, re, im);

    for (i = 0; i < n / 2; i++) {
        float tmp1 = src[i] * inv_n;
        float tmp2 = src[n - i - 1] * inv_n;
        float csc = (0.5f / (float)sin(M_PI / (2.0 * n) * (double)(2 * i + 1))) *
                    (tmp1 - tmp2);

        tmp1 += tmp2;
        dst[i] = tmp1 + csc;
        dst[n - i - 1] = tmp1 - csc;
    }
}

/* Samples are in the unit interval. FFmpeg's C converter is lrintf(x * 32768). */
static int16_t float_to_s16(float x)
{
    long v = lrintf(x * 32768.0f);

    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

static int read_bits(BinkBits *bits, int count, int *err)
{
    int value;

    if (*err)
        return 0;
    value = bink_bits_read(bits, count);
    if (value < 0)
        *err = 1;
    return value;
}

static float read_float(BinkBits *bits, int *err)
{
    int power = read_bits(bits, 5, err);
    int mantissa = read_bits(bits, 23, err);
    int sign = read_bits(bits, 1, err);
    float value = ldexpf((float)mantissa, power - 23);

    if (sign)
        value = -value;
    return value;
}

static int align32_exact(BinkBits *bits)
{
    size_t total = bits->size * 8;
    size_t next = (bits->bitpos + 31) & ~(size_t)31;

    if (next > total)
        return -1;
    bits->bitpos = next;
    return 0;
}

static void setup_bands(BinkAudioInfo *info)
{
    unsigned long half = (info->rate + 1UL) / 2UL;
    int i;

    info->num_bands = 1;
    while (info->num_bands < 25) {
        if (half <= bink_wma_critical_freqs[info->num_bands - 1])
            break;
        info->num_bands++;
    }
    info->bands[0] = 2;
    for (i = 1; i < info->num_bands; i++)
        info->bands[i] =
            (bink_wma_critical_freqs[i - 1] * (unsigned)info->frame_len /
             (unsigned)half) & ~1U;
    info->bands[info->num_bands] = (unsigned)info->frame_len;
}

int bink_audio_describe(unsigned long rate, int channels, int use_dct,
                        BinkAudioInfo *info)
{
    int bits;
    int bits_extra = 0;
    int i;

    if (info == NULL || channels < 1 || channels > 2)
        return -1;
    memset(info, 0, sizeof(*info));
    info->use_dct = use_dct;
    if (!use_dct) {
        /* Bands see rate * channels. The transform itself is one channel. */
        info->rate = rate * (unsigned long)channels;
        if (channels == 2)
            bits_extra = 1;
        info->channels = 1;
    } else {
        info->rate = rate;
        info->channels = channels;
    }

    if (rate < 22050)
        bits = 9;
    else if (rate < 44100)
        bits = 10;
    else
        bits = 11;
    bits += bits_extra;
    info->frame_len = 1 << bits;
    if (info->frame_len > BINK_AUDIO_MAX_FRAME)
        return -1;
    info->overlap_len = info->frame_len / 16;
    if (use_dct)
        info->root = (float)((double)info->frame_len /
                             (sqrt((double)info->frame_len) * 32768.0));
    else
        info->root = (float)(2.0 / (sqrt((double)info->frame_len) * 32768.0));
    for (i = 0; i < 96; i++)
        info->quant[i] =
            expf((float)i * 0.15289164787221953823f) * info->root;
    setup_bands(info);
    return 0;
}

int bink_audio_create(BinkAudio **out, unsigned long rate, int channels,
                      int use_dct, unsigned long max_bytes)
{
    BinkAudio *audio;
    BinkAudioInfo info;

    if (out == NULL || bink_audio_describe(rate, channels, use_dct, &info) != 0)
        return -1;
    audio = (BinkAudio *)calloc(1, sizeof(*audio));
    if (audio == NULL)
        return -1;
    audio->use_dct = info.use_dct;
    audio->max_bytes = max_bytes;
    audio->rate = info.rate;
    audio->channels = info.channels;
    audio->frame_len = info.frame_len;
    audio->overlap_len = info.overlap_len;
    audio->num_bands = info.num_bands;
    audio->root = info.root;
    memcpy(audio->quant_table, info.quant, sizeof(audio->quant_table));
    memcpy(audio->bands, info.bands, sizeof(audio->bands));

    audio->previous = (float *)calloc(
        (size_t)audio->channels * (size_t)audio->overlap_len, sizeof(float));
    audio->block = (float *)calloc(
        (size_t)audio->channels * (size_t)audio->frame_len, sizeof(float));
    audio->pcm = (unsigned char *)malloc(max_bytes ? (size_t)max_bytes : 1);
    audio->fft_re = (double *)malloc((size_t)audio->frame_len * sizeof(double));
    audio->fft_im = (double *)malloc((size_t)audio->frame_len * sizeof(double));
    if (audio->previous == NULL || audio->block == NULL || audio->pcm == NULL ||
        audio->fft_re == NULL || audio->fft_im == NULL) {
        bink_audio_destroy(audio);
        return -1;
    }
    bink_audio_reset(audio);
    *out = audio;
    return 0;
}

void bink_audio_reset(BinkAudio *audio)
{
    if (audio == NULL)
        return;
    audio->first_block = 1;
    audio->pcm_bytes = 0;
    if (audio->previous != NULL)
        memset(audio->previous, 0,
               (size_t)audio->channels * (size_t)audio->overlap_len *
                   sizeof(float));
}

void bink_audio_destroy(BinkAudio *audio)
{
    if (audio == NULL)
        return;
    free(audio->previous);
    free(audio->block);
    free(audio->pcm);
    free(audio->fft_re);
    free(audio->fft_im);
    free(audio);
}

static int decode_dct_block(BinkAudio *audio, BinkBits *bits, int16_t *dst,
                            int *err)
{
    float coeffs[BINK_AUDIO_MAX_FRAME + 2];
    float quant[25];
    int ch;
    int block_out = audio->frame_len - audio->overlap_len;
    int count = audio->overlap_len * audio->channels;

    memset(coeffs, 0, sizeof(coeffs));
    if (audio->use_dct)
        read_bits(bits, 2, err);
    for (ch = 0; ch < audio->channels; ch++) {
        float *out = audio->block + ch * audio->frame_len;
        float *prev = audio->previous + ch * audio->overlap_len;
        int k = 0;
        float q;
        int i;

        coeffs[0] = read_float(bits, err) * audio->root;
        coeffs[1] = read_float(bits, err) * audio->root;
        for (i = 0; i < audio->num_bands; i++) {
            int value = read_bits(bits, 8, err);

            if (value > 95)
                value = 95;
            quant[i] = audio->quant_table[value];
        }
        q = quant[0];
        i = 2;
        while (i < audio->frame_len && !*err) {
            int j;
            int width;

            if (read_bits(bits, 1, err))
                j = i + bink_rle_length_tab[read_bits(bits, 4, err)] * 8;
            else
                j = i + 8;
            if (j > audio->frame_len)
                j = audio->frame_len;
            width = read_bits(bits, 4, err);
            if (width == 0) {
                memset(coeffs + i, 0, (size_t)(j - i) * sizeof(float));
                i = j;
                while (k < audio->num_bands && (int)audio->bands[k] < i)
                    q = quant[k++];
            } else {
                while (i < j && !*err) {
                    int coeff;

                    if (k < audio->num_bands && (int)audio->bands[k] == i)
                        q = quant[k++];
                    coeff = read_bits(bits, width, err);
                    if (coeff) {
                        if (read_bits(bits, 1, err))
                            coeffs[i] = -q * (float)coeff;
                        else
                            coeffs[i] = q * (float)coeff;
                    } else {
                        coeffs[i] = 0.0f;
                    }
                    i++;
                }
            }
        }
        if (*err)
            return -1;
        if (audio->use_dct) {
            coeffs[0] *= 2.0f;
            dct_iii(out, coeffs, audio->frame_len, audio->fft_re, audio->fft_im);
        } else {
            memcpy(out, coeffs, (size_t)audio->frame_len * sizeof(float));
            rdft_dft_c2r(out, audio->frame_len, audio->fft_re, audio->fft_im);
        }
        if (!audio->first_block) {
            int j = ch;

            for (i = 0; i < audio->overlap_len; i++, j += audio->channels)
                out[i] = (prev[i] * (float)(count - j) +
                          out[i] * (float)j) / (float)count;
        }
        memcpy(prev, out + audio->frame_len - audio->overlap_len,
               (size_t)audio->overlap_len * sizeof(float));
    }

    for (ch = 0; ch < block_out; ch++) {
        int c;

        for (c = 0; c < audio->channels; c++) {
            float sample = audio->block[c * audio->frame_len + ch];

            dst[ch * audio->channels + c] = float_to_s16(sample);
        }
    }
    audio->first_block = 0;
    return 0;
}

int bink_audio_decode(BinkAudio *audio, const unsigned char *data,
                      unsigned long size)
{
    BinkBits bits;
    float *saved_prev = NULL;
    int saved_first;
    unsigned long produced = 0;
    int block_bytes;
    int err = 0;

    if (audio == NULL)
        return -1;
    if (size == 0) {
        audio->pcm_bytes = 0;
        return 0;
    }

    saved_first = audio->first_block;
    saved_prev = (float *)malloc((size_t)audio->channels *
                                 (size_t)audio->overlap_len * sizeof(float));
    if (saved_prev == NULL) {
        audio->pcm_bytes = 0;
        return -1;
    }
    memcpy(saved_prev, audio->previous,
           (size_t)audio->channels * (size_t)audio->overlap_len * sizeof(float));

    bink_bits_init(&bits, data, (size_t)size);
    if (bink_bits_left(&bits) < 32)
        err = 1;
    else
        bink_bits_skip(&bits, 32);

    block_bytes = (audio->frame_len - audio->overlap_len) * audio->channels *
                  (int)sizeof(int16_t);
    while (!err && bink_bits_left(&bits) > 0) {
        if (produced + (unsigned long)block_bytes > audio->max_bytes) {
            err = 1;
            break;
        }
        if (decode_dct_block(audio, &bits,
                             (int16_t *)(audio->pcm + produced), &err) != 0) {
            err = 1;
            break;
        }
        produced += (unsigned long)block_bytes;
        if (bink_bits_left(&bits) == 0)
            break;
        if (align32_exact(&bits) != 0)
            err = 1;
    }

    if (err || produced == 0) {
        memcpy(audio->previous, saved_prev,
               (size_t)audio->channels * (size_t)audio->overlap_len *
                   sizeof(float));
        audio->first_block = saved_first;
        audio->pcm_bytes = 0;
        free(saved_prev);
        return -1;
    }
    audio->pcm_bytes = produced;
    free(saved_prev);
    return 0;
}

const unsigned char *bink_audio_pcm(const BinkAudio *audio)
{
    if (audio == NULL || audio->pcm_bytes == 0)
        return NULL;
    return audio->pcm;
}

unsigned long bink_audio_pcm_bytes(const BinkAudio *audio)
{
    if (audio == NULL)
        return 0;
    return audio->pcm_bytes;
}

static int write_float(BinkBitWriter *bw, float value)
{
    int sign = 0;
    int exp;
    int mantissa;
    float mag;

    if (value < 0.0f) {
        sign = 1;
        value = -value;
    }
    if (value == 0.0f)
        return bink_bw_put(bw, 0, 29);

    mag = frexpf(value, &exp);
    mantissa = (int)lrintf(mag * 8388608.0f);
    if (mantissa >= 8388608) {
        mantissa >>= 1;
        exp++;
    }
    while (exp < 0 && mantissa > 0) {
        mantissa >>= 1;
        exp++;
    }
    if (exp < 0)
        exp = 0;
    if (exp > 31) {
        exp = 31;
        mantissa = 0x7fffff;
    }
    if (bink_bw_put(bw, (unsigned)exp, 5) != 0)
        return -1;
    if (bink_bw_put(bw, (unsigned)mantissa, 23) != 0)
        return -1;
    return bink_bw_put(bw, (unsigned)sign, 1);
}

/* Response of dct_iii at the middle sample when src[0] is 1. */
static int dc_response(int n, float *gain)
{
    static float cached[13];
    static unsigned char ready[13];
    int bits = 0;
    int value = n;
    float *src;
    float *dst;
    double *re;
    double *im;

    while (value > 1) {
        value >>= 1;
        bits++;
    }
    if (n < 2 || n != (1 << bits) || bits >= 13)
        return -1;
    if (!ready[bits]) {
        src = (float *)calloc((size_t)n, sizeof(float));
        dst = (float *)calloc((size_t)n, sizeof(float));
        re = (double *)calloc((size_t)n, sizeof(double));
        im = (double *)calloc((size_t)n, sizeof(double));
        if (src == NULL || dst == NULL || re == NULL || im == NULL) {
            free(src);
            free(dst);
            free(re);
            free(im);
            return -1;
        }
        src[0] = 1.0f;
        dct_iii(dst, src, n, re, im);
        cached[bits] = dst[n / 2];
        ready[bits] = 1;
        free(src);
        free(dst);
        free(re);
        free(im);
    }
    if (cached[bits] == 0.0f)
        return -1;
    *gain = cached[bits];
    return 0;
}

int bink_audio_write_dc_block(BinkBitWriter *bw, const BinkAudioInfo *info,
                              const float *channel_mean)
{
    int ch;
    int i;
    float gain = 1.0f;

    if (bw == NULL || info == NULL || channel_mean == NULL)
        return -1;
    if (info->use_dct) {
        if (dc_response(info->frame_len, &gain) != 0)
            return -1;
        if (bink_bw_put(bw, 0, 2) != 0)
            return -1;
    }
    for (ch = 0; ch < info->channels; ch++) {
        float coded = 0.0f;

        if (info->use_dct)
            coded = channel_mean[ch] / (gain * info->root * 2.0f);
        if (write_float(bw, coded) != 0)
            return -1;
        if (write_float(bw, 0.0f) != 0)
            return -1;
        for (i = 0; i < info->num_bands; i++) {
            if (bink_bw_put(bw, 0, 8) != 0)
                return -1;
        }
        i = 2;
        while (i < info->frame_len) {
            if (bink_bw_put(bw, 0, 1) != 0)
                return -1;
            if (bink_bw_put(bw, 0, 4) != 0)
                return -1;
            i += 8;
        }
    }
    return 0;
}
