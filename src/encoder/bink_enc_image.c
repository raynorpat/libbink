#include "bink_enc_image.h"

#include "bink_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bink_enc_status fail(char *err, size_t errlen, const char *message)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", message);
    return BINK_ENC_ERR_INPUT;
}

uint8_t *bink_enc_stbi_load(const char *path, int *width, int *height,
                            const char **reason);
void bink_enc_stbi_free(uint8_t *pixels);

static int ends_with(const char *path, const char *ext)
{
    size_t path_len = strlen(path);
    size_t ext_len = strlen(ext);
    size_t i;

    if (path_len < ext_len)
        return 0;
    for (i = 0; i < ext_len; i++) {
        char a = path[path_len - ext_len + i];
        char b = ext[i];

        if (tolower((unsigned char)a) != tolower((unsigned char)b))
            return 0;
    }
    return 1;
}

static int ppm_next(FILE *fp)
{
    int ch;

    for (;;) {
        ch = fgetc(fp);
        if (ch == '#') {
            while (ch != '\n' && ch != EOF)
                ch = fgetc(fp);
            continue;
        }
        if (ch == EOF || !isspace(ch))
            return ch;
    }
}

static int ppm_uint(FILE *fp, unsigned *out)
{
    int ch = ppm_next(fp);
    unsigned value = 0;
    int digits = 0;

    if (ch == EOF || !isdigit(ch))
        return -1;
    while (isdigit(ch)) {
        value = value * 10u + (unsigned)(ch - '0');
        digits++;
        ch = fgetc(fp);
    }
    if (digits == 0)
        return -1;
    if (ch != EOF)
        ungetc(ch, fp);
    *out = value;
    return 0;
}

static bink_enc_status load_ppm(FILE *fp, uint8_t **rgb, unsigned *width,
                                unsigned *height, char *err, size_t errlen)
{
    char magic[2];
    unsigned w;
    unsigned h;
    unsigned maxval;
    size_t pixels;
    uint8_t *data;

    if (fread(magic, 1, 2, fp) != 2 || magic[0] != 'P' || magic[1] != '6')
        return fail(err, errlen, "PPM must be binary P6");
    if (ppm_uint(fp, &w) != 0 || ppm_uint(fp, &h) != 0 ||
        ppm_uint(fp, &maxval) != 0)
        return fail(err, errlen, "invalid PPM header");
    if (maxval != 255)
        return fail(err, errlen, "PPM maxval must be 255");
    if (w == 0 || h == 0 || w > BINK_MAX_WIDTH || h > BINK_MAX_HEIGHT)
        return fail(err, errlen, "PPM dimensions are out of range");
    /* PPM separates maxval from the pixels with a single whitespace byte. */
    if (fgetc(fp) == EOF)
        return fail(err, errlen, "truncated PPM header");
    pixels = (size_t)w * (size_t)h;
    data = (uint8_t *)malloc(pixels * 3);
    if (data == NULL)
        return fail(err, errlen, "out of memory");
    if (fread(data, 1, pixels * 3, fp) != pixels * 3) {
        free(data);
        return fail(err, errlen, "truncated PPM pixels");
    }
    *rgb = data;
    *width = w;
    *height = h;
    return BINK_ENC_OK;
}

static bink_enc_status load_stb(const char *path, uint8_t **rgb,
                                unsigned *width, unsigned *height,
                                char *err, size_t errlen)
{
    int w = 0;
    int h = 0;
    const char *reason = NULL;
    uint8_t *pixels = bink_enc_stbi_load(path, &w, &h, &reason);

    if (pixels == NULL) {
        if (reason == NULL || reason[0] == '\0')
            return fail(err, errlen, "cannot decode image");
        return fail(err, errlen, reason);
    }
    if (w <= 0 || h <= 0 || (unsigned)w > BINK_MAX_WIDTH ||
        (unsigned)h > BINK_MAX_HEIGHT) {
        bink_enc_stbi_free(pixels);
        return fail(err, errlen, "image dimensions are out of range");
    }
    *rgb = pixels;
    *width = (unsigned)w;
    *height = (unsigned)h;
    return BINK_ENC_OK;
}

bink_enc_status bink_enc_image_load(const char *path, uint8_t **rgb,
                                    unsigned *width, unsigned *height,
                                    char *err, size_t errlen)
{
    FILE *fp;
    bink_enc_status status;

    if (path == NULL || rgb == NULL || width == NULL || height == NULL)
        return fail(err, errlen, "missing image path");
    fp = fopen(path, "rb");
    if (fp == NULL)
        return fail(err, errlen, "cannot open image");
    if (ends_with(path, ".png") || ends_with(path, ".jpg") ||
        ends_with(path, ".jpeg") || ends_with(path, ".bmp")) {
        fclose(fp);
        return load_stb(path, rgb, width, height, err, errlen);
    }
    if (ends_with(path, ".ppm"))
        status = load_ppm(fp, rgb, width, height, err, errlen);
    else
        status = fail(err, errlen,
                      "image must be .ppm, .bmp, .png, .jpg, or .jpeg");
    fclose(fp);
    return status;
}
