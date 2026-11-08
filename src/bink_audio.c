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

#include "bink_audio.h"

#include <stdlib.h>

struct BinkAudio {
    unsigned long rate;
    int channels;
    int use_dct;
    unsigned long max_bytes;
    int first_block;
    const unsigned char *pcm;
    unsigned long pcm_bytes;
};

int bink_audio_create(BinkAudio **out, unsigned long rate, int channels,
                      int use_dct, unsigned long max_bytes)
{
    BinkAudio *audio;

    if (out == NULL)
        return -1;
    audio = (BinkAudio *)calloc(1, sizeof(*audio));
    if (audio == NULL)
        return -1;
    audio->rate = rate;
    audio->channels = channels;
    audio->use_dct = use_dct;
    audio->max_bytes = max_bytes;
    bink_audio_reset(audio);
    *out = audio;
    return 0;
}

void bink_audio_reset(BinkAudio *audio)
{
    if (audio == NULL)
        return;
    audio->first_block = 1;
    audio->pcm = NULL;
    audio->pcm_bytes = 0;
}

void bink_audio_destroy(BinkAudio *audio)
{
    free(audio);
}

int bink_audio_decode(BinkAudio *audio, const unsigned char *data,
                      unsigned long size)
{
    if (audio == NULL)
        return -1;
    if (size == 0) {
        audio->pcm = NULL;
        audio->pcm_bytes = 0;
        return 0;
    }
    (void)data;
    audio->pcm = NULL;
    audio->pcm_bytes = 0;
    return -1;
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
