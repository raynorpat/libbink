/**
 * libbink - Bink Audio decoder
 *
 * Copyright (c) 2007-2011 Peter Ross (pross@xvid.org)
 * Copyright (c) 2009 Daniel Verkamp (daniel@drv.nu)
 *
 * This file is part of libbink, derived from FFmpeg's libavcodec/binkaudio.c.
 * It is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 */

#ifndef LIBBINK_BINK_AUDIO_H
#define LIBBINK_BINK_AUDIO_H

#include "bink_bits.h"

typedef struct BinkAudio BinkAudio;

/** Frame geometry and quantizers shared by the decoder and the encoder. */
typedef struct {
    unsigned long rate;
    int channels;
    int use_dct;
    int frame_len;
    int overlap_len;
    int num_bands;
    unsigned int bands[26];
    float root;
    float quant[96];
} BinkAudioInfo;

int bink_audio_describe(unsigned long rate, int channels, int use_dct,
                        BinkAudioInfo *info);
int bink_audio_create(BinkAudio **out, unsigned long rate, int channels,
                      int use_dct, unsigned long max_bytes);
/**
 * Cosine table for the forward DCT-II of one DCT block:
 * tab[k * frame_len + t] = cos(pi / frame_len * (t + 0.5) * k).
 * Returns frame_len * frame_len floats, or NULL. Free with free().
 */
float *bink_audio_dct_table(int frame_len);
/**
 * Write one DCT block: the 2-bit marker, then per channel the two float
 * coded coefficients, the band quantizer indices, and the coefficient
 * chunks. out[ch] holds frame_len samples (block plus overlap tail) in the
 * -1..1 range, the same time-domain block the decoder reconstructs.
 */
int bink_audio_write_dct_block(BinkBitWriter *bw, const BinkAudioInfo *info,
                               const float *const out[2], const float *tab);
void bink_audio_reset(BinkAudio *audio);
void bink_audio_destroy(BinkAudio *audio);
int bink_audio_decode(BinkAudio *audio, const unsigned char *data,
                      unsigned long size);
const unsigned char *bink_audio_pcm(const BinkAudio *audio);
unsigned long bink_audio_pcm_bytes(const BinkAudio *audio);

#endif
