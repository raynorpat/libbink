#include "bink_enc_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bink_enc_status fail(char *err, size_t errlen, const char *message)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", message);
    return BINK_ENC_ERR_INPUT;
}

static unsigned read_u16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned read_u32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

bink_enc_status bink_enc_wav_load(const char *path, bink_pcm_audio *audio,
                                  char *err, size_t errlen)
{
    FILE *fp;
    unsigned char header[12];
    long file_len;
    unsigned rate = 0;
    unsigned channels = 0;
    unsigned bits = 0;
    int have_fmt = 0;
    unsigned char *data = NULL;
    unsigned data_size = 0;
    size_t frames;
    size_t i;
    int16_t *pcm;

    if (path == NULL || audio == NULL)
        return fail(err, errlen, "missing WAV path");
    memset(audio, 0, sizeof(*audio));
    fp = fopen(path, "rb");
    if (fp == NULL)
        return fail(err, errlen, "cannot open WAV");
    if (fseek(fp, 0, SEEK_END) != 0 || (file_len = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return fail(err, errlen, "cannot read WAV");
    }
    if (fread(header, 1, 12, fp) != 12 || memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0) {
        fclose(fp);
        return fail(err, errlen, "not a WAV file");
    }

    for (;;) {
        unsigned char chunk[8];
        unsigned size;
        long body;
        long next;

        if (fread(chunk, 1, 8, fp) != 8)
            break;
        size = read_u32(chunk + 4);
        body = ftell(fp);
        if (body < 0 || body > file_len)
            break;
        /* A chunk that claims more than the file holds would wrap a 32-bit
         * long and seek backwards, re-reading the same chunk forever. */
        if ((unsigned long)size > (unsigned long)(file_len - body)) {
            if (memcmp(chunk, "data", 4) == 0 && data == NULL) {
                fclose(fp);
                return fail(err, errlen, "truncated WAV data");
            }
            break;
        }
        next = body + (long)size;
        if ((size & 1u) && next < file_len)
            next++;
        if (memcmp(chunk, "fmt ", 4) == 0) {
            unsigned char fmt[16];

            if (size < 16 || fread(fmt, 1, 16, fp) != 16) {
                fclose(fp);
                return fail(err, errlen, "truncated WAV fmt chunk");
            }
            if (read_u16(fmt) != 1) {
                fclose(fp);
                return fail(err, errlen, "WAV must be PCM");
            }
            channels = read_u16(fmt + 2);
            rate = read_u32(fmt + 4);
            bits = read_u16(fmt + 14);
            have_fmt = 1;
        } else if (memcmp(chunk, "data", 4) == 0 && data == NULL) {
            data = (unsigned char *)malloc(size ? size : 1);
            if (data == NULL) {
                fclose(fp);
                return fail(err, errlen, "out of memory");
            }
            if (fread(data, 1, size, fp) != size) {
                free(data);
                fclose(fp);
                return fail(err, errlen, "truncated WAV data");
            }
            data_size = size;
        }
        if (fseek(fp, next, SEEK_SET) != 0)
            break;
    }
    fclose(fp);

    if (!have_fmt || data == NULL) {
        free(data);
        return fail(err, errlen, "WAV is missing fmt or data");
    }
    if ((channels != 1 && channels != 2) || rate == 0 || rate > 65535 ||
        (bits != 8 && bits != 16 && bits != 24)) {
        free(data);
        return fail(err, errlen, "unsupported WAV format");
    }

    {
        unsigned block = channels * (bits / 8);

        if (block == 0 || data_size < block) {
            free(data);
            return fail(err, errlen, "WAV data is empty");
        }
        frames = data_size / block;
    }
    pcm = (int16_t *)malloc(frames * channels * sizeof(int16_t));
    if (pcm == NULL) {
        free(data);
        return fail(err, errlen, "out of memory");
    }
    if (bits == 8) {
        for (i = 0; i < frames * channels; i++)
            pcm[i] = (int16_t)(((int)data[i] - 128) << 8);
    } else if (bits == 16) {
        for (i = 0; i < frames * channels; i++) {
            int value = (int)data[i * 2] | ((int)data[i * 2 + 1] << 8);

            if (value & 0x8000)
                value -= 0x10000;
            pcm[i] = (int16_t)value;
        }
    } else {
        fprintf(stderr, "warning: 24-bit WAV reduced to 16-bit\n");
        audio->reduced_from_24 = 1;
        for (i = 0; i < frames * channels; i++) {
            int value = (int)data[i * 3] | ((int)data[i * 3 + 1] << 8) |
                        ((int)data[i * 3 + 2] << 16);
            int rounded;

            if (value & 0x800000)
                value -= 0x1000000;
            rounded = value >= 0 ? (value + 128) >> 8 : -((-value + 128) >> 8);
            if (rounded > 32767)
                rounded = 32767;
            if (rounded < -32768)
                rounded = -32768;
            pcm[i] = (int16_t)rounded;
        }
    }
    free(data);
    audio->rate = rate;
    audio->channels = channels;
    audio->bits = 16;
    audio->sample_count = frames;
    audio->pcm = pcm;
    return BINK_ENC_OK;
}
