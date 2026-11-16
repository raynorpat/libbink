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
static const int MAX_ERROR = 32;
static const double MAX_RMSE = 5.0;

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
    unsigned track;
    char status;
    bink_enc_status result = BINK_ENC_OK;
    unsigned cw;
    unsigned ch;

    (void)options;
    memset(audio, 0, sizeof(audio));
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
        }
        if (frames + 1 < source->frame_count) {
            status = bink_next(movie);
        }
    }

done:
    for (track = 0; track < BINK_ENC_MAX_AUDIO_TRACKS; track++)
        bink_enc_audio_close(audio[track]);
    free(rgb);
    free(y);
    free(u);
    free(v);
    bink_close(movie);
    return result;
}
