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

typedef struct BinkAudio BinkAudio;

int bink_audio_create(BinkAudio **out, unsigned long rate, int channels,
                      int use_dct, unsigned long max_bytes);
void bink_audio_reset(BinkAudio *audio);
void bink_audio_destroy(BinkAudio *audio);
int bink_audio_decode(BinkAudio *audio, const unsigned char *data,
                      unsigned long size);
const unsigned char *bink_audio_pcm(const BinkAudio *audio);
unsigned long bink_audio_pcm_bytes(const BinkAudio *audio);

#endif
