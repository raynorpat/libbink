/*
 * Bink 1 bundle readers.
 *
 * Ported from FFmpeg libavcodec/bink.c.
 * Copyright (c) 2006-2007 Konstantin Shishkov
 * Copyright (c) 2007-2008 Peter Ross
 * Licensed under the GNU Lesser General Public License, version 2.1 or later.
 */

#include "bink_video.h"

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
