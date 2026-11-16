/**
    bink_build - encode a Bink 1 file from still images
    Copyright (C) 2026 libbink contributors

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.
*/

#include "bink_enc_image.h"
#include "bink_enc_wav.h"
#include "bink_encoder.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define BINK_BUILD_VERSION "0.1.0"

typedef struct {
    char **paths;
    unsigned count;
    unsigned width;
    unsigned height;
    bink_pcm_audio audio;
    int has_audio;
} build_input;

static void usage(FILE *out, const char *program)
{
    fprintf(out,
        "usage: %s <input> [-o output.bik] [options]\n"
        "\n"
        "  <input>                 directory of .ppm/.bmp/.png/.jpg/.jpeg frames, or one image\n"
        "  -o, --output FILE       output .bik (default: <input>.bik)\n"
        "      --audio FILE.wav    PCM audio for the image input\n"
        "      --fps N[/D]         frame rate (default 15)\n"
        "      --keyframe-interval N  frames between keyframes (default 150, 0 = frame 0 only)\n"
        "      --motion-range N    motion search radius 0..15 (default 8)\n"
        "      --verify            decode the result and compare YUV planes and audio\n"
        "      --verbose           print progress\n"
        "      --quiet             suppress progress messages\n"
        "  -h, --help              show this help\n"
        "      --version           show the version\n",
        program);
}

static int usage_error(const char *program, const char *message)
{
    fprintf(stderr, "bink_build: %s\n", message);
    usage(stderr, program);
    return BINK_ENC_ERR_USAGE;
}

static void set_err(char *err, size_t errlen, const char *message)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, "%s", message);
}

static int parse_uint(const char *text, unsigned *out)
{
    char *end;
    unsigned long value = strtoul(text, &end, 10);

    if (end == text || *end != '\0' || value > 0xffffffffUL)
        return -1;
    *out = (unsigned)value;
    return 0;
}

static int parse_fps(const char *text, unsigned *num, unsigned *den)
{
    char *end;
    unsigned long n;
    unsigned long d = 1;

    n = strtoul(text, &end, 10);
    if (end == text || n == 0 || n > 1000000UL)
        return -1;
    if (*end == '/') {
        const char *start = end + 1;

        d = strtoul(start, &end, 10);
        if (end == start || d == 0 || d > 1000000UL)
            return -1;
    }
    if (*end != '\0')
        return -1;
    *num = (unsigned)n;
    *den = (unsigned)d;
    return 0;
}

static void derive_output(const char *input, char *out, size_t outlen)
{
    const char *base = input;
    const char *dot;
    size_t length;
    size_t i;

    for (i = 0; input[i] != '\0'; i++) {
        if (input[i] == '/' || input[i] == '\\')
            base = input + i + 1;
    }
    if (*base == '\0')
        base = "out";
    dot = strrchr(base, '.');
    length = dot != NULL && dot != base ? (size_t)(dot - base) : strlen(base);
    if (length > outlen - 5)
        length = outlen - 5;
    memcpy(out, base, length);
    memcpy(out + length, ".bik", 5);
}

static int ends_with_ci(const char *name, const char *ext)
{
    size_t name_len = strlen(name);
    size_t ext_len = strlen(ext);
    size_t i;

    if (name_len < ext_len)
        return 0;
    for (i = 0; i < ext_len; i++) {
        unsigned char a = (unsigned char)name[name_len - ext_len + i];
        unsigned char b = (unsigned char)ext[i];

        if (tolower(a) != tolower(b))
            return 0;
    }
    return 1;
}

static int has_image_extension(const char *name)
{
    return ends_with_ci(name, ".ppm") || ends_with_ci(name, ".bmp") ||
           ends_with_ci(name, ".png") || ends_with_ci(name, ".jpg") ||
           ends_with_ci(name, ".jpeg");
}

static char *duplicate_string(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = (char *)malloc(length);

    if (copy != NULL)
        memcpy(copy, text, length);
    return copy;
}

static char *join_path(const char *dir, const char *name)
{
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    int need_sep = dir_len > 0 && dir[dir_len - 1] != '/' &&
                   dir[dir_len - 1] != '\\';
    char *out = (char *)malloc(dir_len + name_len + 2);

    if (out == NULL)
        return NULL;
    memcpy(out, dir, dir_len);
    if (need_sep)
        out[dir_len++] = '/';
    memcpy(out + dir_len, name, name_len + 1);
    return out;
}

/* Compare strings with embedded decimal numbers, e.g. frame2 < frame10. */
static int natural_compare(const char *a, const char *b)
{
    size_t i = 0;
    size_t j = 0;

    while (a[i] != '\0' && b[j] != '\0') {
        if (isdigit((unsigned char)a[i]) && isdigit((unsigned char)b[j])) {
            size_t a_start;
            size_t b_start;
            int cmp;

            while (a[i] == '0')
                i++;
            while (b[j] == '0')
                j++;
            a_start = i;
            b_start = j;
            while (isdigit((unsigned char)a[i]))
                i++;
            while (isdigit((unsigned char)b[j]))
                j++;
            if (i - a_start != j - b_start)
                return i - a_start < j - b_start ? -1 : 1;
            cmp = memcmp(a + a_start, b + b_start, i - a_start);
            if (cmp != 0)
                return cmp < 0 ? -1 : 1;
        } else {
            int ca = tolower((unsigned char)a[i]);
            int cb = tolower((unsigned char)b[j]);

            if (ca != cb)
                return ca < cb ? -1 : 1;
            i++;
            j++;
        }
    }
    if (a[i] != '\0')
        return 1;
    if (b[j] != '\0')
        return -1;
    return 0;
}

static int name_compare(const void *left, const void *right)
{
    const char *a = *(const char *const *)left;
    const char *b = *(const char *const *)right;
    int cmp = natural_compare(a, b);

    if (cmp != 0)
        return cmp;
    return strcmp(a, b);
}

static int path_is_dir(const char *path)
{
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(path);

    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat info;

    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

static int path_exists(const char *path)
{
#if defined(_WIN32)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat info;

    return stat(path, &info) == 0;
#endif
}

static void free_names(char **names, unsigned count)
{
    unsigned i;

    if (names == NULL)
        return;
    for (i = 0; i < count; i++)
        free(names[i]);
    free(names);
}

static bink_enc_status push_name(char ***names, unsigned *count,
                                 unsigned *capacity, const char *name)
{
    if (*count == *capacity) {
        unsigned next = *capacity == 0 ? 16u : *capacity * 2u;
        char **grown = (char **)realloc(*names, (size_t)next * sizeof(char *));

        if (grown == NULL)
            return BINK_ENC_ERR_ENCODE;
        *names = grown;
        *capacity = next;
    }
    (*names)[*count] = duplicate_string(name);
    if ((*names)[*count] == NULL)
        return BINK_ENC_ERR_ENCODE;
    (*count)++;
    return BINK_ENC_OK;
}

static bink_enc_status collect_names(const char *dir, char ***out_names,
                                     unsigned *out_count, char *err,
                                     size_t errlen)
{
    char **names = NULL;
    unsigned count = 0;
    unsigned capacity = 0;
    bink_enc_status status = BINK_ENC_OK;

#if defined(_WIN32)
    {
        char pattern[4096];
        WIN32_FIND_DATAA entry;
        HANDLE handle;

        snprintf(pattern, sizeof(pattern), "%s\\*", dir);
        handle = FindFirstFileA(pattern, &entry);
        if (handle == INVALID_HANDLE_VALUE) {
            set_err(err, errlen, "cannot open image directory");
            return BINK_ENC_ERR_INPUT;
        }
        do {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (!has_image_extension(entry.cFileName))
                continue;
            status = push_name(&names, &count, &capacity, entry.cFileName);
            if (status != BINK_ENC_OK) {
                FindClose(handle);
                free_names(names, count);
                set_err(err, errlen, "out of memory reading image directory");
                return status;
            }
        } while (FindNextFileA(handle, &entry));
        FindClose(handle);
    }
#else
    {
        DIR *handle = opendir(dir);
        struct dirent *entry;

        if (handle == NULL) {
            set_err(err, errlen, "cannot open image directory");
            return BINK_ENC_ERR_INPUT;
        }
        while ((entry = readdir(handle)) != NULL) {
            struct stat info;
            char *full;

            if (entry->d_name[0] == '.')
                continue;
            if (!has_image_extension(entry->d_name))
                continue;
            full = join_path(dir, entry->d_name);
            if (full == NULL) {
                closedir(handle);
                free_names(names, count);
                set_err(err, errlen, "out of memory reading image directory");
                return BINK_ENC_ERR_ENCODE;
            }
            if (stat(full, &info) != 0 || !S_ISREG(info.st_mode)) {
                free(full);
                continue;
            }
            free(full);
            status = push_name(&names, &count, &capacity, entry->d_name);
            if (status != BINK_ENC_OK) {
                closedir(handle);
                free_names(names, count);
                set_err(err, errlen, "out of memory reading image directory");
                return status;
            }
        }
        closedir(handle);
    }
#endif

    if (count == 0) {
        free(names);
        set_err(err, errlen, "no PPM, BMP, PNG, or JPEG images in directory");
        return BINK_ENC_ERR_INPUT;
    }
    qsort(names, count, sizeof(char *), name_compare);
    *out_names = names;
    *out_count = count;
    return BINK_ENC_OK;
}

static void input_close(build_input *input)
{
    unsigned i;

    if (input == NULL)
        return;
    if (input->paths != NULL) {
        for (i = 0; i < input->count; i++)
            free(input->paths[i]);
        free(input->paths);
    }
    free(input->audio.pcm);
    memset(input, 0, sizeof(*input));
}

static int input_read_frame(void *user, unsigned index, uint8_t *rgb)
{
    build_input *input = (build_input *)user;
    uint8_t *loaded = NULL;
    unsigned width = 0;
    unsigned height = 0;
    char message[256];

    if (input == NULL || rgb == NULL || index >= input->count)
        return -1;
    if (bink_enc_image_load(input->paths[index], &loaded, &width, &height,
                            message, sizeof(message)) != BINK_ENC_OK)
        return -1;
    if (width != input->width || height != input->height) {
        free(loaded);
        return -1;
    }
    memcpy(rgb, loaded, (size_t)width * height * 3);
    free(loaded);
    return 0;
}

static size_t input_read_audio(void *user, unsigned track, size_t start_sample,
                               size_t sample_count, void *dst)
{
    build_input *input = (build_input *)user;
    size_t available;
    size_t bytes;

    if (input == NULL || !input->has_audio || track != 0 || dst == NULL)
        return 0;
    if (start_sample >= input->audio.sample_count)
        return 0;
    available = input->audio.sample_count - start_sample;
    if (available > sample_count)
        available = sample_count;
    bytes = available * input->audio.channels * sizeof(int16_t);
    memcpy(dst, input->audio.pcm + start_sample * input->audio.channels,
           bytes);
    return bytes;
}

static bink_enc_status input_open(const char *path, int directory,
                                  build_input *input, char *err, size_t errlen)
{
    char **names = NULL;
    unsigned count = 0;
    unsigned i;
    uint8_t *probe = NULL;
    unsigned width = 0;
    unsigned height = 0;
    bink_enc_status status;

    memset(input, 0, sizeof(*input));
    if (directory) {
        status = collect_names(path, &names, &count, err, errlen);
        if (status != BINK_ENC_OK)
            return status;
    } else {
        if (!has_image_extension(path)) {
            set_err(err, errlen,
                    "image must be .ppm, .bmp, .png, .jpg, or .jpeg");
            return BINK_ENC_ERR_INPUT;
        }
        names = (char **)malloc(sizeof(char *));
        if (names == NULL) {
            set_err(err, errlen, "out of memory");
            return BINK_ENC_ERR_ENCODE;
        }
        names[0] = duplicate_string(path);
        if (names[0] == NULL) {
            free(names);
            set_err(err, errlen, "out of memory");
            return BINK_ENC_ERR_ENCODE;
        }
        count = 1;
    }

    input->paths = (char **)calloc(count, sizeof(char *));
    if (input->paths == NULL) {
        free_names(names, count);
        set_err(err, errlen, "out of memory");
        return BINK_ENC_ERR_ENCODE;
    }
    for (i = 0; i < count; i++) {
        input->paths[i] = directory ? join_path(path, names[i])
                                    : duplicate_string(names[i]);
        if (input->paths[i] == NULL) {
            input->count = i;
            input_close(input);
            free_names(names, count);
            set_err(err, errlen, "out of memory");
            return BINK_ENC_ERR_ENCODE;
        }
        input->count = i + 1;
    }
    free_names(names, count);

    status = bink_enc_image_load(input->paths[0], &probe, &width, &height,
                                 err, errlen);
    free(probe);
    if (status != BINK_ENC_OK) {
        input_close(input);
        return status;
    }
    input->width = width;
    input->height = height;
    return BINK_ENC_OK;
}

static bink_enc_status input_attach_audio(build_input *input, const char *path,
                                          char *err, size_t errlen)
{
    bink_enc_status status = bink_enc_wav_load(path, &input->audio, err,
                                               errlen);

    if (status != BINK_ENC_OK)
        return status;
    input->has_audio = 1;
    return BINK_ENC_OK;
}

int main(int argc, char **argv)
{
    bink_enc_options options;
    bink_enc_source source;
    bink_enc_status status;
    build_input input;
    const char *image_input = NULL;
    const char *output = NULL;
    const char *audio = NULL;
    char output_buffer[4096];
    char err[512];
    unsigned fps_num = 15;
    unsigned fps_den = 1;
    int i;

    bink_enc_options_default(&options);
    memset(&input, 0, sizeof(input));
    memset(&source, 0, sizeof(source));
    err[0] = '\0';

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout, argv[0]);
            return BINK_ENC_OK;
        }
        if (strcmp(arg, "--version") == 0) {
            printf("bink_build %s\n", BINK_BUILD_VERSION);
            return BINK_ENC_OK;
        }
        if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
            if (++i >= argc)
                return usage_error(argv[0], "--output needs a value");
            output = argv[i];
        } else if (strcmp(arg, "--audio") == 0) {
            if (++i >= argc)
                return usage_error(argv[0], "--audio needs a value");
            audio = argv[i];
        } else if (strcmp(arg, "--fps") == 0) {
            if (++i >= argc || parse_fps(argv[i], &fps_num, &fps_den) != 0)
                return usage_error(argv[0], "--fps expects N or N/D");
        } else if (strcmp(arg, "--keyframe-interval") == 0) {
            if (++i >= argc ||
                parse_uint(argv[i], &options.keyframe_interval) != 0)
                return usage_error(argv[0],
                                   "--keyframe-interval expects a number");
        } else if (strcmp(arg, "--motion-range") == 0) {
            if (++i >= argc || parse_uint(argv[i], &options.motion_range) != 0 ||
                options.motion_range > 15)
                return usage_error(argv[0], "--motion-range expects 0..15");
        } else if (strcmp(arg, "--verify") == 0) {
            options.verify = 1;
        } else if (strcmp(arg, "--verbose") == 0) {
            options.verbose = 1;
        } else if (strcmp(arg, "--quiet") == 0) {
            options.verbose = 0;
        } else if (arg[0] == '-') {
            fprintf(stderr, "bink_build: unknown option %s\n", arg);
            return BINK_ENC_ERR_USAGE;
        } else if (image_input == NULL) {
            image_input = arg;
        } else {
            return usage_error(argv[0], "only one input is accepted");
        }
    }

    if (image_input == NULL)
        return usage_error(argv[0], "missing input");
    if (!path_exists(image_input)) {
        fprintf(stderr, "bink_build: cannot open input\n");
        return BINK_ENC_ERR_INPUT;
    }
    if (output == NULL) {
        derive_output(image_input, output_buffer, sizeof(output_buffer));
        output = output_buffer;
    }

    status = input_open(image_input, path_is_dir(image_input), &input, err,
                        sizeof(err));
    if (status == BINK_ENC_OK && audio != NULL)
        status = input_attach_audio(&input, audio, err, sizeof(err));
    if (status != BINK_ENC_OK) {
        if (err[0] != '\0')
            fprintf(stderr, "bink_build: %s\n", err);
        input_close(&input);
        return status;
    }

    source.user = &input;
    source.width = input.width;
    source.height = input.height;
    source.frame_count = input.count;
    source.fps_num = fps_num;
    source.fps_den = fps_den;
    source.read_frame = input_read_frame;
    source.read_audio = input.has_audio ? input_read_audio : NULL;
    if (input.has_audio) {
        source.audio_track_count = 1;
        source.audio[0].rate = input.audio.rate;
        source.audio[0].channels = input.audio.channels;
        source.audio[0].bits = input.audio.bits;
        source.audio[0].sample_count = input.audio.sample_count;
    }

    if (options.verbose)
        fprintf(stderr, "bink_build: encoding %s\n", image_input);
    status = bink_enc_encode(&source, &options, output, err, sizeof(err));
    input_close(&input);
    if (status != BINK_ENC_OK) {
        if (err[0] != '\0')
            fprintf(stderr, "bink_build: %s\n", err);
        return status;
    }
    if (options.verbose)
        fprintf(stderr, "bink_build: wrote %s\n", output);
    return BINK_ENC_OK;
}
