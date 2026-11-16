#include "bink_enc_verify.h"

#include "bink_enc_audio.h"
#include "bink_enc_video.h"
#include "bink.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per-plane tolerance: exact output passes trivially, DCT blocks must stay
 * inside these caps. */
static const unsigned MAX_ERROR = 32;
static const double MAX_RMSE = 5.0;

/* Audio is lossy too. The decoded track's RMS error must stay within this
 * fraction of the source's RMS level, with an absolute floor so near-silent
 * input is not held to a relative bound. Sine sources measure about 0.4%;
 * a swapped channel, a shifted block or a dropped packet lands near 100%. */
static const double MAX_AUDIO_ERROR_RATIO = 0.25;
static const double MAX_AUDIO_ERROR_FLOOR = 64.0;

typedef struct {
    size_t pos;      /* next source frame the decoder should produce */
    double error;    /* sum of squared differences */
    double signal;   /* sum of squared source samples */
    double count;    /* samples compared */
} AudioCheck;

static bink_enc_status fail(char *err, size_t errlen, const char *message)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", message);
    return BINK_ENC_ERR_VERIFY;
}

static int planes_within_tolerance(const bink movie, const uint8_t *y,
                                   const uint8_t *u, const uint8_t *v,
                                   unsigned width, unsigned height, char *err,
                                   size_t errlen)
{
    static const char *const names[3] = { "Y", "U", "V" };
    unsigned cw = (width + 1) >> 1;
    unsigned ch = (height + 1) >> 1;
    const unsigned char *planes[3];
    const uint8_t *expect[3];
    unsigned dims[3][2];
    int i;

    planes[0] = bink_get_plane(movie, BINK_PLANE_Y);
    planes[1] = bink_get_plane(movie, BINK_PLANE_U);
    planes[2] = bink_get_plane(movie, BINK_PLANE_V);
    expect[0] = y;
    expect[1] = u;
    expect[2] = v;
    dims[0][0] = width;
    dims[0][1] = height;
    dims[1][0] = cw;
    dims[1][1] = ch;
    dims[2][0] = cw;
    dims[2][1] = ch;
    for (i = 0; i < 3; i++) {
        int stride = bink_get_stride(movie, i);
        unsigned row;
        unsigned col;
        unsigned count = dims[i][0] * dims[i][1];
        unsigned max_error = 0;
        double sum = 0.0;
        double rmse;

        if (planes[i] == NULL || stride <= 0) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "%s plane is unavailable", names[i]);
            return 0;
        }
        for (row = 0; row < dims[i][1]; row++) {
            for (col = 0; col < dims[i][0]; col++) {
                int diff =
                    (int)planes[i][(size_t)row * (size_t)stride + col] -
                    (int)expect[i][(size_t)row * dims[i][0] + col];
                unsigned error = (unsigned)(diff < 0 ? -diff : diff);

                if (error > max_error)
                    max_error = error;
                sum += (double)diff * (double)diff;
            }
        }
        if (count == 0) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen, "%s plane is empty", names[i]);
            return 0;
        }
        rmse = sqrt(sum / (double)count);
        if (max_error > MAX_ERROR || rmse > MAX_RMSE) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen,
                         "%s plane exceeds tolerance (max error %u, RMSE %.2f)",
                         names[i], max_error, rmse);
            return 0;
        }
    }
    return 1;
}

/* Compare one frame's decoded PCM for `track` with the source samples at
 * the same position, treating anything past the source as silence. */
static int accumulate_audio(const bink movie, const bink_enc_source *source,
                            unsigned track, AudioCheck *check,
                            int16_t **scratch, size_t *scratch_cap)
{
    const unsigned char *pcm = bink_get_audio(movie, (unsigned char)track);
    unsigned long bytes = bink_get_audio_size(movie, (unsigned char)track);
    size_t channels = source->audio[track].channels;
    size_t total = source->audio[track].sample_count;
    size_t frames;
    size_t samples;
    size_t i;

    if (pcm == NULL || bytes == 0 || channels == 0)
        return 0;
    frames = (size_t)bytes / (channels * sizeof(int16_t));
    samples = frames * channels;
    if (samples > *scratch_cap) {
        int16_t *grown =
            (int16_t *)realloc(*scratch, samples * sizeof(int16_t));

        if (grown == NULL)
            return -1;
        *scratch = grown;
        *scratch_cap = samples;
    }
    memset(*scratch, 0, samples * sizeof(int16_t));
    if (check->pos < total && source->read_audio != NULL) {
        size_t want = total - check->pos;

        if (want > frames)
            want = frames;
        source->read_audio(source->user, track, check->pos, want, *scratch);
    }
    for (i = 0; i < samples; i++) {
        int16_t got;
        double expect = (double)(*scratch)[i];
        double diff;

        memcpy(&got, pcm + i * sizeof(int16_t), sizeof(got));
        diff = (double)got - expect;
        check->error += diff * diff;
        check->signal += expect * expect;
    }
    check->count += (double)samples;
    check->pos += frames;
    return 0;
}

bink_enc_status bink_enc_verify(const char *path, const bink_enc_source *source,
                                const bink_enc_options *options,
                                char *err, size_t errlen)
{
    bink movie;
    unsigned long frames = 0;
    unsigned long width = 0;
    unsigned long height = 0;
    unsigned char track_mask = 0;
    unsigned char channels[7];
    unsigned char bitdepth[7];
    unsigned long rates[7];
    uint8_t *rgb = NULL;
    uint8_t *y = NULL;
    uint8_t *u = NULL;
    uint8_t *v = NULL;
    BinkEncAudio *audio[BINK_ENC_MAX_AUDIO_TRACKS];
    AudioCheck checks[BINK_ENC_MAX_AUDIO_TRACKS];
    int16_t *scratch = NULL;
    size_t scratch_cap = 0;
    unsigned track;
    char status;
    bink_enc_status result = BINK_ENC_OK;
    unsigned cw;
    unsigned ch;

    (void)options;
    memset(audio, 0, sizeof(audio));
    memset(checks, 0, sizeof(checks));
    memset(channels, 0, sizeof(channels));
    memset(bitdepth, 0, sizeof(bitdepth));
    memset(rates, 0, sizeof(rates));

    movie = bink_open_file(path, BINK_MODE_DISK);
    if (movie == NULL)
        return fail(err, errlen, "decoder rejected the encoded file");
    if (bink_info_all(movie, NULL, &frames, NULL) != 0 ||
        bink_info_video(movie, &width, &height, NULL) != 0 ||
        bink_info_audio(movie, &track_mask, channels, bitdepth, rates) != 0) {
        bink_close(movie);
        return fail(err, errlen, "cannot read encoded file info");
    }
    if (frames != source->frame_count || width != source->width ||
        height != source->height) {
        bink_close(movie);
        return fail(err, errlen, "encoded file metadata does not match");
    }
    for (track = 0; track < source->audio_track_count; track++) {
        unsigned long max_decoded = 0;

        if ((track_mask & (1u << track)) == 0 ||
            channels[track] != source->audio[track].channels ||
            bitdepth[track] != 16 ||
            rates[track] != source->audio[track].rate) {
            bink_close(movie);
            return fail(err, errlen, "encoded audio metadata does not match");
        }
        if (bink_enc_audio_open(&audio[track], &source->audio[track],
                                source->fps_num, source->fps_den,
                                source->frame_count, &max_decoded) != 0 ||
            bink_enable_audio(movie, (unsigned char)track, 1) != 0) {
            result = fail(err, errlen, "cannot verify audio");
            goto done;
        }
    }
    if (bink_enable_video(movie, 1) != 0) {
        result = fail(err, errlen, "cannot enable video");
        goto done;
    }

    cw = (source->width + 1) >> 1;
    ch = (source->height + 1) >> 1;
    rgb = (uint8_t *)malloc((size_t)source->width * source->height * 3);
    y = (uint8_t *)malloc((size_t)source->width * source->height);
    u = (uint8_t *)malloc((size_t)cw * ch);
    v = (uint8_t *)malloc((size_t)cw * ch);
    if (rgb == NULL || y == NULL || u == NULL || v == NULL) {
        result = fail(err, errlen, "out of memory");
        goto done;
    }

    status = bink_first(movie);
    for (frames = 0; frames < source->frame_count; frames++) {
        if (status != BINK_MORE && status != BINK_LAST) {
            result = fail(err, errlen, "decoder stopped early");
            goto done;
        }
        if (source->read_frame(source->user, frames, rgb) != 0) {
            result = fail(err, errlen, "cannot reread a frame during verify");
            goto done;
        }
        bink_enc_rgb_to_yuv(rgb, source->width, source->height, y, u, v);
        if (!planes_within_tolerance(movie, y, u, v, source->width,
                                     source->height, err, errlen)) {
            result = BINK_ENC_ERR_VERIFY;
            goto done;
        }
        for (track = 0; track < source->audio_track_count; track++) {
            size_t expect = 0;
            int is_last = frames + 1 == source->frame_count;

            if (bink_enc_audio_next_size(audio[track], is_last, &expect) != 0) {
                result = fail(err, errlen, "audio schedule failed during verify");
                goto done;
            }
            if (bink_get_audio_size(movie, (unsigned char)track) !=
                (unsigned long)expect) {
                result = fail(err, errlen, "decoded audio size does not match");
                goto done;
            }
            if (accumulate_audio(movie, source, track, &checks[track],
                                 &scratch, &scratch_cap) != 0) {
                result = fail(err, errlen, "out of memory");
                goto done;
            }
        }
        if (frames + 1 < source->frame_count) {
            status = bink_next(movie);
        }
    }

    for (track = 0; track < source->audio_track_count; track++) {
        const AudioCheck *check = &checks[track];
        double rmse;
        double level;
        double cap;

        if (check->count <= 0.0)
            continue;
        rmse = sqrt(check->error / check->count);
        level = sqrt(check->signal / check->count);
        cap = level * MAX_AUDIO_ERROR_RATIO;
        if (cap < MAX_AUDIO_ERROR_FLOOR)
            cap = MAX_AUDIO_ERROR_FLOOR;
        if (rmse > cap) {
            if (err != NULL && errlen > 0)
                snprintf(err, errlen,
                         "audio track %u exceeds tolerance (RMSE %.1f, "
                         "source level %.1f)",
                         track, rmse, level);
            result = BINK_ENC_ERR_VERIFY;
            goto done;
        }
    }

done:
    for (track = 0; track < BINK_ENC_MAX_AUDIO_TRACKS; track++)
        bink_enc_audio_close(audio[track]);
    free(scratch);
    free(rgb);
    free(y);
    free(u);
    free(v);
    bink_close(movie);
    return result;
}
