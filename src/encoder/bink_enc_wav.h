#ifndef BINK_ENC_WAV_H
#define BINK_ENC_WAV_H

#include "bink_encoder.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned rate;
    unsigned channels;
    unsigned bits;
    size_t sample_count;
    int16_t *pcm;
    int reduced_from_24;
} bink_pcm_audio;

/* Load PCM WAV as interleaved host-endian s16. The caller frees audio->pcm. */
bink_enc_status bink_enc_wav_load(const char *path, bink_pcm_audio *audio,
                                  char *err, size_t errlen);

#endif
