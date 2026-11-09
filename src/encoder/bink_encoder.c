#include "bink_encoder.h"

#include "bink_enc_audio.h"
#include "bink_enc_verify.h"
#include "bink_enc_video.h"
#include "bink_bits.h"
#include "bink_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bink_enc_status fail(char *err, size_t errlen, bink_enc_status status,
                            const char *message)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", message);
    return status;
}

static int is_keyframe(unsigned index, unsigned interval)
{
    if (index == 0)
        return 1;
    if (interval == 0)
        return 0;
    return (index % interval) == 0;
}

static int write_u16(FILE *fp, unsigned value)
{
    unsigned char bytes[2];

    bytes[0] = (unsigned char)(value & 255u);
    bytes[1] = (unsigned char)((value >> 8) & 255u);
    return fwrite(bytes, 1, 2, fp) == 2 ? 0 : -1;
}

static int write_u32(FILE *fp, unsigned long value)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char)(value & 255u);
    bytes[1] = (unsigned char)((value >> 8) & 255u);
    bytes[2] = (unsigned char)((value >> 16) & 255u);
    bytes[3] = (unsigned char)((value >> 24) & 255u);
    return fwrite(bytes, 1, 4, fp) == 4 ? 0 : -1;
}

void bink_enc_options_default(bink_enc_options *options)
{
    if (options == NULL)
        return;
    memset(options, 0, sizeof(*options));
    options->keyframe_interval = 150;
    options->motion_range = 8;
}

bink_enc_status bink_enc_encode(const bink_enc_source *source,
                                const bink_enc_options *options,
                                const char *output_path, char *err,
                                size_t errlen)
{
    bink_enc_options local;
    FILE *fp = NULL;
    BinkEncPlanes planes;
    BinkEncAudio *audio[BINK_ENC_MAX_AUDIO_TRACKS];
    unsigned long max_decoded[BINK_ENC_MAX_AUDIO_TRACKS];
    unsigned long *offsets = NULL;
    unsigned char *keyframes = NULL;
    uint8_t *rgb = NULL;
    uint8_t *y = NULL;
    uint8_t *u = NULL;
    uint8_t *v = NULL;
    long index_pos = 0;
    unsigned long largest = 0;
    unsigned long frame;
    unsigned track;
    unsigned cw;
    unsigned ch;
    int planes_ready = 0;
    int created = 0;
    bink_enc_status status = BINK_ENC_OK;

    memset(&planes, 0, sizeof(planes));
    memset(audio, 0, sizeof(audio));
    memset(max_decoded, 0, sizeof(max_decoded));

    if (source == NULL || output_path == NULL || source->read_frame == NULL)
        return fail(err, errlen, BINK_ENC_ERR_ENCODE, "missing encode source");
    if (options == NULL) {
        bink_enc_options_default(&local);
        options = &local;
    }
    if (source->width == 0 || source->height == 0 || source->frame_count == 0 ||
        source->width > BINK_MAX_WIDTH || source->height > BINK_MAX_HEIGHT ||
        source->frame_count > BINK_MAX_FRAMES)
        return fail(err, errlen, BINK_ENC_ERR_INPUT, "frame size is out of range");
    if (source->fps_num == 0 || source->fps_den == 0)
        return fail(err, errlen, BINK_ENC_ERR_INPUT, "frame rate must be non-zero");
    if (source->audio_track_count > BINK_ENC_MAX_AUDIO_TRACKS)
        return fail(err, errlen, BINK_ENC_ERR_INPUT, "too many audio tracks");
    if (options->motion_range > 15)
        return fail(err, errlen, BINK_ENC_ERR_INPUT, "motion range must be 0..15");

    cw = (source->width + 1) >> 1;
    ch = (source->height + 1) >> 1;
    rgb = (uint8_t *)malloc((size_t)source->width * source->height * 3);
    y = (uint8_t *)malloc((size_t)source->width * source->height);
    u = (uint8_t *)malloc((size_t)cw * ch);
    v = (uint8_t *)malloc((size_t)cw * ch);
    offsets = (unsigned long *)calloc(source->frame_count, sizeof(*offsets));
    keyframes = (unsigned char *)calloc(source->frame_count, 1);
    if (rgb == NULL || y == NULL || u == NULL || v == NULL || offsets == NULL ||
        keyframes == NULL ||
        bink_enc_planes_init(&planes, source->width, source->height) != 0) {
        status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "out of memory");
        goto done;
    }
    planes_ready = 1;

    for (track = 0; track < source->audio_track_count; track++) {
        if (bink_enc_audio_open(&audio[track], &source->audio[track],
                                source->fps_num, source->fps_den,
                                source->frame_count, &max_decoded[track]) != 0) {
            status = fail(err, errlen, BINK_ENC_ERR_INPUT,
                          "unsupported audio track");
            goto done;
        }
    }

    fp = fopen(output_path, "wb+");
    if (fp == NULL) {
        status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot create output");
        goto done;
    }
    created = 1;
    if (fwrite("BIKi", 1, 4, fp) != 4 || write_u32(fp, 0) != 0 ||
        write_u32(fp, source->frame_count) != 0 || write_u32(fp, 0) != 0 ||
        write_u32(fp, 0) != 0 || write_u32(fp, source->width) != 0 ||
        write_u32(fp, source->height) != 0 ||
        write_u32(fp, source->fps_num) != 0 ||
        write_u32(fp, source->fps_den) != 0 || write_u32(fp, 0) != 0 ||
        write_u32(fp, source->audio_track_count) != 0) {
        status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot write header");
        goto done;
    }
    for (track = 0; track < source->audio_track_count; track++) {
        if (write_u32(fp, max_decoded[track]) != 0) {
            status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot write header");
            goto done;
        }
    }
    for (track = 0; track < source->audio_track_count; track++) {
        unsigned flags = 0x1000u;

        if (source->audio[track].channels == 2)
            flags |= 0x2000u;
        if (write_u16(fp, source->audio[track].rate) != 0 ||
            write_u16(fp, flags) != 0) {
            status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot write header");
            goto done;
        }
    }
    for (track = 0; track < source->audio_track_count; track++) {
        if (write_u32(fp, 0) != 0) {
            status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot write header");
            goto done;
        }
    }
    index_pos = ftell(fp);
    if (index_pos < 0) {
        status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot write header");
        goto done;
    }
    for (frame = 0; frame < source->frame_count; frame++) {
        if (write_u32(fp, 0) != 0) {
            status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot write index");
            goto done;
        }
    }

    for (frame = 0; frame < source->frame_count; frame++) {
        BinkBitWriter video;
        long start;
        long end;
        int key = is_keyframe(frame, options->keyframe_interval);
        uint8_t *payloads[BINK_ENC_MAX_AUDIO_TRACKS];
        size_t payload_sizes[BINK_ENC_MAX_AUDIO_TRACKS];
        size_t pcm_bytes = 0;

        memset(payloads, 0, sizeof(payloads));
        memset(payload_sizes, 0, sizeof(payload_sizes));
        if (source->read_frame(source->user, frame, rgb) != 0) {
            status = fail(err, errlen, BINK_ENC_ERR_INPUT, "cannot read frame");
            goto done;
        }
        bink_enc_rgb_to_yuv(rgb, source->width, source->height, y, u, v);
        bink_bw_init(&video);
        if (bink_enc_write_frame(&video, &planes, y, u, v, key,
                                 (int)options->motion_range) != 0) {
            bink_bw_free(&video);
            status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot encode frame");
            goto done;
        }
        for (track = 0; track < source->audio_track_count; track++) {
            if (bink_enc_audio_frame(audio[track], source, track,
                                     frame + 1 == source->frame_count,
                                     &payloads[track], &payload_sizes[track],
                                     &pcm_bytes) != 0) {
                bink_bw_free(&video);
                status = fail(err, errlen, BINK_ENC_ERR_ENCODE,
                              "cannot encode audio");
                for (track = 0; track < source->audio_track_count; track++)
                    free(payloads[track]);
                goto done;
            }
        }

        start = ftell(fp);
        if (start < 0 || (start & 1) || start > 0x7ffffffeL) {
            bink_bw_free(&video);
            for (track = 0; track < source->audio_track_count; track++)
                free(payloads[track]);
            status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "frame offset overflow");
            goto done;
        }
        offsets[frame] = (unsigned long)start;
        keyframes[frame] = (unsigned char)key;
        for (track = 0; track < source->audio_track_count; track++) {
            if (write_u32(fp, (unsigned long)payload_sizes[track]) != 0 ||
                (payload_sizes[track] != 0 &&
                 fwrite(payloads[track], 1, payload_sizes[track], fp) !=
                     payload_sizes[track])) {
                bink_bw_free(&video);
                for (track = 0; track < source->audio_track_count; track++)
                    free(payloads[track]);
                status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot write audio");
                goto done;
            }
            free(payloads[track]);
            payloads[track] = NULL;
        }
        if (write_u32(fp, (unsigned long)bink_bw_bytes(&video)) != 0 ||
            fwrite(bink_bw_data(&video), 1, bink_bw_bytes(&video), fp) !=
                bink_bw_bytes(&video)) {
            bink_bw_free(&video);
            status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot write video");
            goto done;
        }
        bink_bw_free(&video);
        end = ftell(fp);
        if (end < 0) {
            status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot write frame");
            goto done;
        }
        if (end & 1) {
            if (fputc(0, fp) == EOF) {
                status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot pad frame");
                goto done;
            }
            end++;
        }
        if ((unsigned long)(end - start) > largest)
            largest = (unsigned long)(end - start);
        if (options->verbose)
            fprintf(stderr, "frame %lu/%u %s\n",
                    (unsigned long)frame + 1, source->frame_count,
                    key ? "key" : "delta");
    }

    {
        long end = ftell(fp);

        if (end < 8 || fseek(fp, 4, SEEK_SET) != 0 ||
            write_u32(fp, (unsigned long)end - 8) != 0 ||
            fseek(fp, 12, SEEK_SET) != 0 || write_u32(fp, largest) != 0 ||
            fseek(fp, index_pos, SEEK_SET) != 0) {
            status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot patch header");
            goto done;
        }
        for (frame = 0; frame < source->frame_count; frame++) {
            unsigned long word = offsets[frame];

            if (keyframes[frame])
                word |= 1ul;
            if (write_u32(fp, word) != 0) {
                status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot patch index");
                goto done;
            }
        }
    }
    if (fclose(fp) != 0) {
        fp = NULL;
        status = fail(err, errlen, BINK_ENC_ERR_ENCODE, "cannot close output");
        goto done;
    }
    fp = NULL;

    if (options->verify) {
        status = bink_enc_verify(output_path, source, options, err, errlen);
        if (status != BINK_ENC_OK)
            goto done;
    }

done:
    if (fp != NULL)
        fclose(fp);
    if (created && status != BINK_ENC_OK && status != BINK_ENC_ERR_VERIFY)
        remove(output_path);
    if (planes_ready)
        bink_enc_planes_free(&planes);
    for (track = 0; track < BINK_ENC_MAX_AUDIO_TRACKS; track++)
        bink_enc_audio_close(audio[track]);
    free(offsets);
    free(keyframes);
    free(rgb);
    free(y);
    free(u);
    free(v);
    return status;
}
