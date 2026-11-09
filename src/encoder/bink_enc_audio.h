#ifndef BINK_ENC_AUDIO_H
#define BINK_ENC_AUDIO_H

#include "bink_encoder.h"

#include <stddef.h>
#include <stdint.h>

typedef struct BinkEncAudio BinkEncAudio;

int bink_enc_audio_open(BinkEncAudio **out, const bink_enc_audio_info *info,
                        unsigned fps_num, unsigned fps_den,
                        unsigned long frames, unsigned long *max_decoded);
void bink_enc_audio_close(BinkEncAudio *audio);

/*
 * Encode the next video frame's audio payload. *pcm_bytes is the number of
 * PCM bytes the decoder will publish for this payload. A frame with no
 * samples returns a NULL payload and a size of 0.
 */
int bink_enc_audio_frame(BinkEncAudio *audio,
                         const bink_enc_source *source, unsigned track,
                         int is_last, uint8_t **payload, size_t *payload_size,
                         size_t *pcm_bytes);

/* Same scheduling as bink_enc_audio_frame, without writing a payload. */
int bink_enc_audio_next_size(BinkEncAudio *audio, int is_last,
                             size_t *pcm_bytes);

#endif
