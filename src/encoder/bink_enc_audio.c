#include "bink_enc_audio.h"

#include "bink_audio.h"
#include "bink_bits.h"

#include <stdlib.h>
#include <string.h>

struct BinkEncAudio {
    BinkAudioInfo info;
    unsigned fps_num;
    unsigned fps_den;
    uint64_t acc;
    uint64_t produced;
    uint64_t target;
    size_t source_pos;
    size_t sample_count;
    int block_out;
    float *out;
    BinkAudioDct *dct;
    int16_t *look;
};

int bink_enc_audio_open(BinkEncAudio **out, const bink_enc_audio_info *info,
                        unsigned fps_num, unsigned fps_den,
                        unsigned long frames, unsigned long *max_decoded)
{
    BinkEncAudio *audio;
    uint64_t acc = 0;
    uint64_t video_samples = 0;
    uint64_t bytes;
    unsigned long i;

    if (out == NULL || info == NULL || max_decoded == NULL)
        return -1;
    if (info->channels != 1 && info->channels != 2)
        return -1;
    if (info->bits != 16 || info->rate == 0 || info->rate > 65535)
        return -1;
    if (fps_num == 0 || fps_den == 0 || frames == 0)
        return -1;

    audio = (BinkEncAudio *)calloc(1, sizeof(*audio));
    if (audio == NULL)
        return -1;
    if (bink_audio_describe(info->rate, (int)info->channels, 1, &audio->info) !=
        0) {
        free(audio);
        return -1;
    }
    audio->fps_num = fps_num;
    audio->fps_den = fps_den;
    audio->sample_count = info->sample_count;
    audio->block_out = audio->info.frame_len - audio->info.overlap_len;
    if (audio->block_out <= 0) {
        free(audio);
        return -1;
    }
    audio->out = (float *)calloc(
        (size_t)audio->info.channels * (size_t)audio->info.frame_len,
        sizeof(float));
    audio->look = (int16_t *)calloc(
        (size_t)audio->info.channels * (size_t)audio->info.overlap_len,
        sizeof(int16_t));
    audio->dct = bink_audio_dct_open(audio->info.frame_len);
    if (audio->out == NULL || audio->look == NULL || audio->dct == NULL) {
        free(audio->out);
        free(audio->look);
        bink_audio_dct_close(audio->dct);
        free(audio);
        return -1;
    }

    for (i = 0; i < frames; i++) {
        acc += (uint64_t)info->rate * fps_den;
        video_samples += acc / fps_num;
        acc %= fps_num;
    }
    if ((uint64_t)info->sample_count > video_samples)
        video_samples = info->sample_count;
    video_samples += (uint64_t)audio->block_out;
    if (video_samples > (uint64_t)-1 / ((uint64_t)info->channels * 2)) {
        free(audio);
        return -1;
    }
    bytes = video_samples * info->channels * 2;
    if (bytes > 0xffffffffULL) {
        free(audio);
        return -1;
    }
    *max_decoded = (unsigned long)bytes;
    *out = audio;
    return 0;
}

void bink_enc_audio_close(BinkEncAudio *audio)
{
    if (audio == NULL)
        return;
    free(audio->out);
    free(audio->look);
    bink_audio_dct_close(audio->dct);
    free(audio);
}

static int take_blocks(BinkEncAudio *audio, int is_last, int *blocks,
                       size_t *pcm_bytes)
{
    uint64_t quota;
    uint64_t goal;
    uint64_t end;
    int count = 0;

    audio->acc += (uint64_t)audio->info.rate * audio->fps_den;
    quota = audio->acc / audio->fps_num;
    audio->acc %= audio->fps_num;
    audio->target += quota;
    goal = audio->target;
    if (is_last && (uint64_t)audio->sample_count > goal)
        goal = audio->sample_count;
    end = audio->produced;
    while (end < goal) {
        count++;
        end += (uint64_t)audio->block_out;
        if (count > 1000000)
            return -1;
    }
    if ((uint64_t)count * (uint64_t)audio->block_out *
            (uint64_t)audio->info.channels * 2 >
        0xffffffffULL)
        return -1;
    audio->produced = end;
    *blocks = count;
    *pcm_bytes = (size_t)count * (size_t)audio->block_out *
                 (size_t)audio->info.channels * 2;
    return 0;
}

int bink_enc_audio_next_size(BinkEncAudio *audio, int is_last,
                             size_t *pcm_bytes)
{
    int blocks;

    if (audio == NULL || pcm_bytes == NULL)
        return -1;
    return take_blocks(audio, is_last, &blocks, pcm_bytes);
}

int bink_enc_audio_frame(BinkEncAudio *audio, const bink_enc_source *source,
                         unsigned track, int is_last, uint8_t **payload,
                         size_t *payload_size, size_t *pcm_bytes)
{
    BinkBitWriter writer;
    int blocks;
    int block;
    size_t pcm = 0;
    int16_t *samples = NULL;

    if (audio == NULL || payload == NULL || payload_size == NULL ||
        pcm_bytes == NULL)
        return -1;
    *payload = NULL;
    *payload_size = 0;
    *pcm_bytes = 0;
    if (take_blocks(audio, is_last, &blocks, &pcm) != 0)
        return -1;
    *pcm_bytes = pcm;
    if (blocks == 0)
        return 0;

    bink_bw_init(&writer);
    if (bink_bw_put(&writer, (unsigned)pcm, 32) != 0) {
        bink_bw_free(&writer);
        return -1;
    }
    samples = (int16_t *)calloc(
        (size_t)audio->block_out * (size_t)audio->info.channels, sizeof(int16_t));
    if (samples == NULL) {
        bink_bw_free(&writer);
        return -1;
    }

    for (block = 0; block < blocks; block++) {
        const float *out_ptrs[2];
        int ch;
        size_t want = (size_t)audio->block_out;
        size_t ahead;
        int i;

        memset(samples, 0,
               (size_t)audio->block_out * (size_t)audio->info.channels *
                   sizeof(int16_t));
        if (audio->source_pos < audio->sample_count) {
            if (audio->source_pos + want > audio->sample_count)
                want = audio->sample_count - audio->source_pos;
        } else {
            want = 0;
        }
        if (want != 0 && source != NULL && source->read_audio != NULL) {
            source->read_audio(source->user, track, audio->source_pos, want,
                               samples);
        }
        /* One block of lookahead: the tail of this block must equal the head
         * of the next one so the decoder's crossfade is exact. */
        memset(audio->out, 0,
               (size_t)audio->info.channels * (size_t)audio->info.frame_len *
                   sizeof(float));
        memset(audio->look, 0,
               (size_t)audio->info.channels * (size_t)audio->info.overlap_len *
                   sizeof(int16_t));
        ahead = audio->source_pos + (size_t)audio->block_out;
        if (ahead < audio->sample_count && source != NULL &&
            source->read_audio != NULL) {
            size_t count = audio->sample_count - ahead;

            if (count > (size_t)audio->info.overlap_len)
                count = (size_t)audio->info.overlap_len;
            source->read_audio(source->user, track, ahead, count, audio->look);
        }
        for (ch = 0; ch < audio->info.channels; ch++) {
            float *dst = audio->out + (size_t)ch * (size_t)audio->info.frame_len;

            for (i = 0; i < audio->block_out; i++)
                dst[i] = (float)samples[i * audio->info.channels + ch] /
                         32768.0f;
            for (i = 0; i < audio->info.overlap_len; i++)
                dst[audio->block_out + i] =
                    (float)audio->look[i * audio->info.channels + ch] /
                    32768.0f;
        }
        out_ptrs[0] = audio->out;
        out_ptrs[1] = audio->info.channels == 2
                          ? audio->out + audio->info.frame_len
                          : NULL;
        if (bink_audio_write_dct_block(&writer, &audio->info, out_ptrs,
                                       audio->dct) != 0 ||
            bink_bw_align32(&writer) != 0) {
            free(samples);
            bink_bw_free(&writer);
            return -1;
        }
        audio->source_pos += (size_t)audio->block_out;
    }
    free(samples);

    *payload_size = bink_bw_bytes(&writer);
    *payload = (uint8_t *)malloc(*payload_size != 0 ? *payload_size : 1);
    if (*payload == NULL) {
        bink_bw_free(&writer);
        return -1;
    }
    if (*payload_size != 0)
        memcpy(*payload, bink_bw_data(&writer), *payload_size);
    bink_bw_free(&writer);
    return 0;
}
