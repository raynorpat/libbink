/**
    libbink encoder
    Copyright (C) 2026 libbink contributors

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.
*/

#ifndef BINK_ENCODER_H
#define BINK_ENCODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BINK_ENC_MAX_AUDIO_TRACKS 7

/* Status values double as process exit codes. */
typedef enum {
    BINK_ENC_OK = 0,
    BINK_ENC_ERR_USAGE = 1,
    BINK_ENC_ERR_INPUT = 2,
    BINK_ENC_ERR_ENCODE = 3,
    BINK_ENC_ERR_VERIFY = 4
} bink_enc_status;

typedef struct {
    unsigned rate;
    unsigned channels; /* 1 or 2 */
    unsigned bits;     /* 16 */
    size_t sample_count;
} bink_enc_audio_info;

/*
 * Input adapter. read_frame and read_audio may be called more than once per
 * frame (the encoder, then --verify), so they must be repeatable.
 *
 * read_frame fills width * height * 3 bytes of packed RGB.
 * read_audio fills interleaved host-endian s16 PCM for
 * [start_sample, start_sample + sample_count) and returns the number of
 * bytes written. A short read is padded with silence.
 */
typedef struct bink_enc_source {
    void *user;
    unsigned width;
    unsigned height;
    unsigned frame_count;
    unsigned fps_num;
    unsigned fps_den;
    unsigned audio_track_count;
    bink_enc_audio_info audio[BINK_ENC_MAX_AUDIO_TRACKS];
    int (*read_frame)(void *user, unsigned index, uint8_t *rgb);
    size_t (*read_audio)(void *user, unsigned track, size_t start_sample,
                         size_t sample_count, void *dst);
} bink_enc_source;

typedef struct {
    unsigned keyframe_interval; /* 0 = only frame 0 */
    unsigned motion_range;      /* 0..15; 0 disables motion search */
    int verify;
    int verbose;
} bink_enc_options;

void bink_enc_options_default(bink_enc_options *options);

bink_enc_status bink_enc_encode(const bink_enc_source *source,
                                const bink_enc_options *options,
                                const char *output_path,
                                char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
