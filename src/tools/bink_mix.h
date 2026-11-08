/**
    libbink - A C library for decoding Bink 1 video files
    Copyright (C) 2026 libbink contributors

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.
*/

#ifndef LIBBINK_BINK_MIX_H
#define LIBBINK_BINK_MIX_H

#include <stdint.h>

typedef struct {
    const int16_t *pcm;
    unsigned long samples; /* per channel */
    unsigned long rate;
    int channels;
} BinkMixTrack;

/* Write dst_frames frames of out_channels. Return 0, or -1 on a NULL
   destination or a zero rate. */
int bink_mix(const BinkMixTrack *tracks, int count,
             unsigned long out_rate, int out_channels,
             int16_t *dst, unsigned long dst_frames);

#endif
