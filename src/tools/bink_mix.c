/**
    libbink - A C library for decoding Bink 1 video files
    Copyright (C) 2026 libbink contributors

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.
*/

#include "tools/bink_mix.h"

#include <math.h>
#include <stddef.h>

static int clip_s16(int value)
{
    if (value > 32767)
        return 32767;
    if (value < -32768)
        return -32768;
    return value;
}

static int sample_at(const BinkMixTrack *track, unsigned long index,
                     int out_channels, int channel)
{
    if (track->channels == 1)
        return track->pcm[index];
    if (out_channels == 1) {
        int left = track->pcm[index * 2];
        int right = track->pcm[index * 2 + 1];

        return (int)lrintf(((double)left + (double)right) / 2.0);
    }
    return track->pcm[index * 2 + channel];
}

static int resample(const BinkMixTrack *track, unsigned long frame,
                    unsigned long out_rate, int out_channels, int channel)
{
    double pos;
    double frac;
    unsigned long index;
    int left;
    int right;

    pos = (double)frame * (double)track->rate / (double)out_rate;
    if (track->samples == 1 || pos >= (double)(track->samples - 1))
        return sample_at(track, track->samples - 1, out_channels, channel);

    index = (unsigned long)pos;
    frac = pos - (double)index;
    left = sample_at(track, index, out_channels, channel);
    right = sample_at(track, index + 1, out_channels, channel);
    return (int)lrintf((1.0 - frac) * (double)left + frac * (double)right);
}

static int track_contributes(const BinkMixTrack *track)
{
    return track->pcm != NULL && track->samples > 0;
}

int bink_mix(const BinkMixTrack *tracks, int count,
             unsigned long out_rate, int out_channels,
             int16_t *dst, unsigned long dst_frames)
{
    unsigned long frame;
    int i;

    if (dst == NULL || out_rate == 0 || out_channels <= 0)
        return -1;
    if (count < 0 || (count > 0 && tracks == NULL))
        return -1;

    for (i = 0; i < count; i++) {
        if (!track_contributes(&tracks[i]))
            continue;
        if (tracks[i].rate == 0 ||
            (tracks[i].channels != 1 && tracks[i].channels != 2))
            return -1;
    }

    for (frame = 0; frame < dst_frames; frame++) {
        int channel;

        for (channel = 0; channel < out_channels; channel++) {
            int sum = 0;

            for (i = 0; i < count; i++) {
                if (!track_contributes(&tracks[i]))
                    continue;
                sum += resample(&tracks[i], frame, out_rate, out_channels,
                                channel);
            }
            dst[frame * (unsigned long)out_channels + (unsigned long)channel] =
                (int16_t)clip_s16(sum);
        }
    }
    return 0;
}
