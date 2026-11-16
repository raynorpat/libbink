#include "bink.h"
#include "bink_audio.h"
#include "bink_dsp.h"
#include "bink_enc_dct.h"
#include "bink_enc_image.h"
#include "bink_enc_video.h"
#include "bink_enc_wav.h"
#include "bink_encoder.h"
#include "bink_enc_paths.h"
#include "bink_video.h"
#include "test_common.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    uint8_t *frames;
    unsigned width;
    unsigned height;
    unsigned count;
    const int16_t *pcm;
    size_t sample_count;
    unsigned channels;
    int fail_index;
} mem_source;

static int mem_read_frame(void *user, unsigned index, uint8_t *rgb)
{
    mem_source *source = (mem_source *)user;
    size_t bytes;

    if (source->fail_index >= 0 && (int)index == source->fail_index)
        return -1;
    if (index >= source->count || source->frames == NULL)
        return -1;
    bytes = (size_t)source->width * source->height * 3;
    memcpy(rgb, source->frames + (size_t)index * bytes, bytes);
    return 0;
}

static size_t mem_read_audio(void *user, unsigned track, size_t start,
                             size_t count, void *dst)
{
    mem_source *source = (mem_source *)user;
    size_t available;
    size_t bytes;

    if (track != 0 || source->pcm == NULL || start >= source->sample_count)
        return 0;
    available = source->sample_count - start;
    if (available > count)
        available = count;
    bytes = available * source->channels * sizeof(int16_t);
    memcpy(dst, source->pcm + start * source->channels, bytes);
    return bytes;
}

static void path_for(char *out, size_t outlen, const char *name)
{
    snprintf(out, outlen, "%s/%s", BINK_ENC_TMP, name);
}

static bink_enc_status encode_mem(mem_source *mem, const bink_enc_options *opt,
                                  const char *path, unsigned rate, char *err,
                                  size_t errlen)
{
    bink_enc_source source;

    memset(&source, 0, sizeof(source));
    source.user = mem;
    source.width = mem->width;
    source.height = mem->height;
    source.frame_count = mem->count;
    source.fps_num = 15;
    source.fps_den = 1;
    source.read_frame = mem_read_frame;
    if (rate != 0) {
        source.audio_track_count = 1;
        source.audio[0].rate = rate;
        source.audio[0].channels = mem->channels;
        source.audio[0].bits = 16;
        source.audio[0].sample_count = mem->sample_count;
        source.read_audio = mem_read_audio;
    }
    return bink_enc_encode(&source, opt, path, err, errlen);
}

static void fill_rgb(uint8_t *frame, unsigned width, unsigned height,
                     uint8_t r, uint8_t g, uint8_t b)
{
    unsigned i;
    unsigned pixels = width * height;

    for (i = 0; i < pixels; i++) {
        frame[i * 3] = r;
        frame[i * 3 + 1] = g;
        frame[i * 3 + 2] = b;
    }
}

static void fill_checker(uint8_t *frame, unsigned width, unsigned height)
{
    unsigned x;
    unsigned y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint8_t *p = frame + ((size_t)y * width + x) * 3;
            int on = (x ^ y) & 1;

            p[0] = on ? 255 : 0;
            p[1] = on ? 255 : 0;
            p[2] = on ? 255 : 0;
        }
    }
}

static void fill_gradient(uint8_t *frame, unsigned width, unsigned height)
{
    unsigned x;
    unsigned y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint8_t *p = frame + ((size_t)y * width + x) * 3;

            p[0] = (uint8_t)((x * 15) & 255);
            p[1] = (uint8_t)((y * 13) & 255);
            p[2] = (uint8_t)((x * 3 + y * 5) & 255);
        }
    }
}

static void fill_shift(uint8_t *dst, const uint8_t *src, unsigned width,
                       unsigned height, int dx, int dy)
{
    unsigned x;
    unsigned y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int sx = (int)x - dx;
            int sy = (int)y - dy;
            const uint8_t *from;
            uint8_t *to = dst + ((size_t)y * width + x) * 3;

            if (sx < 0)
                sx = 0;
            if (sy < 0)
                sy = 0;
            if (sx >= (int)width)
                sx = (int)width - 1;
            if (sy >= (int)height)
                sy = (int)height - 1;
            from = src + ((size_t)sy * width + (size_t)sx) * 3;
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
        }
    }
}

static int expect_ok(bink_enc_status status, const char *err, const char *what)
{
    if (status == BINK_ENC_OK)
        return 0;
    fprintf(stderr, "FAIL %s: status %d %s\n", what, (int)status, err);
    return 1;
}

/* Decoded planes against the expected planes, with the encoder's block caps. */
static int planes_within(const bink movie, const uint8_t *y, const uint8_t *u,
                         const uint8_t *v, unsigned width, unsigned height,
                         unsigned max_error_cap, double rmse_cap)
{
    static const char *const names[3] = { "Y", "U", "V" };
    const unsigned char *planes[3];
    const uint8_t *expect[3];
    unsigned cw = (width + 1) >> 1;
    unsigned ch = (height + 1) >> 1;
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

        if (planes[i] == NULL || stride <= 0 || count == 0) {
            fprintf(stderr, "FAIL %s plane is unavailable\n", names[i]);
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
        rmse = sqrt(sum / (double)count);
        if (max_error > max_error_cap || rmse > rmse_cap) {
            fprintf(stderr,
                    "FAIL %s plane exceeds tolerance (max error %u, RMSE %.2f)\n",
                    names[i], max_error, rmse);
            return 0;
        }
    }
    return 1;
}

static int test_still(const char *name, const uint8_t *rgb, unsigned width,
                      unsigned height)
{
    mem_source mem;
    bink_enc_options opt;
    char path[1024];
    char err[256];
    bink_enc_status status;

    memset(&mem, 0, sizeof(mem));
    mem.frames = (uint8_t *)rgb;
    mem.width = width;
    mem.height = height;
    mem.count = 1;
    mem.fail_index = -1;
    bink_enc_options_default(&opt);
    opt.verify = 1;
    opt.motion_range = 0;
    path_for(path, sizeof(path), name);
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 0, err, sizeof(err));
    if (expect_ok(status, err, name) != 0)
        return 1;
    remove(path);
    return 0;
}

static int test_pictures(void)
{
    uint8_t solid[16 * 16 * 3];
    uint8_t checker[16 * 16 * 3];
    uint8_t gradient[16 * 16 * 3];
    uint8_t odd[17 * 9 * 3];

    fill_rgb(solid, 16, 16, 220, 30, 40);
    fill_checker(checker, 16, 16);
    fill_gradient(gradient, 16, 16);
    fill_gradient(odd, 17, 9);
    if (test_still("solid.bik", solid, 16, 16) != 0)
        return 1;
    if (test_still("checker.bik", checker, 16, 16) != 0)
        return 1;
    if (test_still("gradient.bik", gradient, 16, 16) != 0)
        return 1;
    if (test_still("odd.bik", odd, 17, 9) != 0)
        return 1;
    return 0;
}

static unsigned long read_u32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int packet_sizes(const char *path, unsigned long *first,
                        unsigned long *second)
{
    FILE *fp = fopen(path, "rb");
    unsigned char hdr[48];
    unsigned long file_size;
    unsigned long frames;
    unsigned long tracks;
    unsigned long off0;
    unsigned long off1;
    unsigned char index[8];

    if (fp == NULL)
        return -1;
    if (fread(hdr, 1, 44, fp) != 44) {
        fclose(fp);
        return -1;
    }
    file_size = read_u32(hdr + 4) + 8;
    frames = read_u32(hdr + 8);
    tracks = read_u32(hdr + 40);
    if (frames < 2 || tracks != 0) {
        fclose(fp);
        return -1;
    }
    if (fread(index, 1, 8, fp) != 8) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    off0 = read_u32(index) & ~1ul;
    off1 = read_u32(index + 4) & ~1ul;
    if (off1 <= off0 || file_size <= off1)
        return -1;
    *first = off1 - off0;
    *second = file_size - off1;
    return 0;
}

static int test_skip_and_motion(void)
{
    enum { W = 32, H = 24 };
    uint8_t *frames;
    mem_source mem;
    bink_enc_options opt;
    char path[1024];
    char err[256];
    bink_enc_status status;
    unsigned long first = 0;
    unsigned long second = 0;
    unsigned x;
    unsigned y;
    size_t frame_bytes = (size_t)W * H * 3;

    frames = (uint8_t *)calloc(2, frame_bytes);
    if (frames == NULL)
        return 1;
    fill_rgb(frames, W, H, 40, 80, 160);
    memcpy(frames + frame_bytes, frames, frame_bytes);

    memset(&mem, 0, sizeof(mem));
    mem.frames = frames;
    mem.width = W;
    mem.height = H;
    mem.count = 2;
    mem.fail_index = -1;
    bink_enc_options_default(&opt);
    opt.verify = 1;
    opt.keyframe_interval = 0;
    opt.motion_range = 8;
    path_for(path, sizeof(path), "skip.bik");
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 0, err, sizeof(err));
    if (expect_ok(status, err, "skip") != 0) {
        free(frames);
        return 1;
    }
    if (packet_sizes(path, &first, &second) != 0 || second >= first) {
        fprintf(stderr, "FAIL skip packet sizes %lu %lu\n", first, second);
        free(frames);
        remove(path);
        return 1;
    }
    remove(path);

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint8_t *p = frames + ((size_t)y * W + x) * 3;

            p[0] = (uint8_t)(x * 8);
            p[1] = (uint8_t)(y * 10);
            p[2] = (uint8_t)(40 + x);
        }
    }
    fill_shift(frames + frame_bytes, frames, W, H, 3, 2);
    path_for(path, sizeof(path), "motion.bik");
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 0, err, sizeof(err));
    free(frames);
    if (expect_ok(status, err, "motion") != 0)
        return 1;
    remove(path);
    return 0;
}

static int test_intra_dct(void)
{
    enum { W = 64, H = 48 };
    mem_source mem;
    bink_enc_options opt;
    char path[1024];
    char err[256];
    bink_enc_status status;
    uint8_t *rgb = (uint8_t *)malloc((size_t)W * H * 3);
    uint8_t *y = (uint8_t *)malloc((size_t)W * H);
    uint8_t *u = (uint8_t *)malloc((size_t)(W / 2) * (H / 2));
    uint8_t *v = (uint8_t *)malloc((size_t)(W / 2) * (H / 2));
    unsigned x;
    unsigned yy;
    bink movie;
    struct stat st;
    char frame_status;
    int rc = 1;

    if (rgb == NULL || y == NULL || u == NULL || v == NULL) {
        fprintf(stderr, "FAIL intra dct: out of memory\n");
        goto done;
    }
    for (yy = 0; yy < H; yy++) {
        for (x = 0; x < W; x++) {
            size_t p = ((size_t)yy * W + x) * 3;

            rgb[p + 0] = (uint8_t)(x * 3 + yy);
            rgb[p + 1] = (uint8_t)(x + yy * 2);
            rgb[p + 2] = (uint8_t)(255 - x - yy);
        }
    }
    memset(&mem, 0, sizeof(mem));
    mem.frames = rgb;
    mem.width = W;
    mem.height = H;
    mem.count = 1;
    mem.fail_index = -1;
    bink_enc_options_default(&opt);
    opt.motion_range = 0;
    path_for(path, sizeof(path), "intra_dct.bik");
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 0, err, sizeof(err));
    if (expect_ok(status, err, "intra dct") != 0)
        goto done;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "FAIL intra dct: no output file\n");
        goto done;
    }
    /* A lossless fallback costs about W*H + 2*(W/2)*(H/2) bytes of block data. */
    if ((long)st.st_size >= (long)(W * H + 2 * (W / 2) * (H / 2))) {
        fprintf(stderr, "FAIL intra dct: file is %ld bytes, not smaller than raw\n",
                (long)st.st_size);
        goto done;
    }
    movie = bink_open_file(path, BINK_MODE_DISK);
    if (movie == NULL) {
        fprintf(stderr, "FAIL intra dct: decoder rejected the file\n");
        goto done;
    }
    bink_enc_rgb_to_yuv(rgb, W, H, y, u, v);
    frame_status = bink_enable_video(movie, 1) != 0 ? 0 : bink_first(movie);
    if (frame_status != BINK_MORE && frame_status != BINK_LAST) {
        fprintf(stderr, "FAIL intra dct: cannot decode the first frame\n");
        goto close_movie;
    }
    if (!planes_within(movie, y, u, v, W, H, 24, 4.0))
        goto close_movie;
    rc = 0;
close_movie:
    bink_close(movie);
done:
    remove(path);
    free(rgb);
    free(y);
    free(u);
    free(v);
    return rc;
}

/* Per-frame packet sizes straight from the file index. Returns the frame
 * count, or -1. */
static int packet_sizes_all(const char *path, unsigned long *sizes, int max)
{
    FILE *fp = fopen(path, "rb");
    unsigned char hdr[48];
    unsigned long offs[32];
    unsigned long file_size;
    unsigned long frames;
    int i;

    if (fp == NULL)
        return -1;
    if (fread(hdr, 1, 44, fp) != 44) {
        fclose(fp);
        return -1;
    }
    file_size = read_u32(hdr + 4) + 8;
    frames = read_u32(hdr + 8);
    if (frames < 1 || frames > (unsigned long)max) {
        fclose(fp);
        return -1;
    }
    for (i = 0; i < (int)frames; i++) {
        unsigned char entry[4];

        if (fread(entry, 1, 4, fp) != 4) {
            fclose(fp);
            return -1;
        }
        offs[i] = read_u32(entry) & ~1ul;
    }
    fclose(fp);
    for (i = 0; i < (int)frames; i++) {
        unsigned long end = i + 1 < (int)frames ? offs[i + 1] : file_size;

        if (end <= offs[i])
            return -1;
        sizes[i] = end - offs[i];
    }
    return (int)frames;
}

/* Decodes every frame and checks it against the matching source frame. */
static int decode_within(bink movie, const uint8_t *frames, int count,
                         unsigned width, unsigned height, uint8_t *y,
                         uint8_t *u, uint8_t *v, unsigned maxerr,
                         double rmse)
{
    size_t bytes = (size_t)width * height * 3;
    char status;
    int i;

    if (bink_enable_video(movie, 1) != 0)
        return 0;
    status = bink_first(movie);
    for (i = 0; i < count; i++) {
        if (status != BINK_MORE && status != BINK_LAST) {
            fprintf(stderr, "FAIL decode: stopped at frame %d\n", i);
            return 0;
        }
        bink_enc_rgb_to_yuv(frames + (size_t)i * bytes, width, height, y, u,
                            v);
        if (!planes_within(movie, y, u, v, width, height, maxerr, rmse))
            return 0;
        if (i + 1 < count)
            status = bink_next(movie);
    }
    return 1;
}

static int test_inter_dct(void)
{
    enum { W = 64, H = 48, N = 3 };
    uint8_t *frames;
    mem_source mem;
    bink_enc_options opt;
    unsigned long sizes[8];
    char path[1024];
    char err[256];
    bink_enc_status status;
    uint8_t *y;
    uint8_t *u;
    uint8_t *v;
    size_t frame_bytes = (size_t)W * H * 3;
    unsigned x;
    unsigned yy;
    bink movie;
    int rc = 1;

    frames = (uint8_t *)calloc((size_t)N, frame_bytes);
    y = (uint8_t *)malloc((size_t)W * H);
    u = (uint8_t *)malloc((size_t)(W / 2) * (H / 2));
    v = (uint8_t *)malloc((size_t)(W / 2) * (H / 2));
    if (frames == NULL || y == NULL || u == NULL || v == NULL) {
        fprintf(stderr, "FAIL inter dct: out of memory\n");
        goto done;
    }
    for (yy = 0; yy < H; yy++) {
        for (x = 0; x < W; x++) {
            uint8_t *p = frames + ((size_t)yy * W + x) * 3;

            /* Fine detail: no shift of this pattern (plus a brightness step)
             * can match exactly, and a flat step is cheap only when the
             * residual is transform coded. */
            p[0] = (uint8_t)((x * 73 + yy * 151 + x * yy * 3) & 255);
            p[1] = (uint8_t)((x * 29 + yy * 47 + x * yy * 5) & 255);
            p[2] = (uint8_t)((x * 11 + yy * 97 + x * yy * 7) & 255);
        }
    }
    {
        int f;
        size_t i;

        for (f = 1; f < N; f++) {
            int add = 2 * f;
            uint8_t *dst = frames + (size_t)f * frame_bytes;

            memcpy(dst, frames, frame_bytes);
            for (i = 0; i < frame_bytes; i++) {
                unsigned value = (unsigned)dst[i] + (unsigned)add;

                dst[i] = (uint8_t)(value > 255 ? 255 : value);
            }
        }
    }

    memset(&mem, 0, sizeof(mem));
    mem.frames = frames;
    mem.width = W;
    mem.height = H;
    mem.count = N;
    mem.fail_index = -1;
    bink_enc_options_default(&opt);
    opt.verify = 1;
    opt.keyframe_interval = 0;
    opt.motion_range = 8;
    path_for(path, sizeof(path), "inter_dct.bik");
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 0, err, sizeof(err));
    if (expect_ok(status, err, "inter dct") != 0)
        goto done;
    if (packet_sizes_all(path, sizes, 8) != N) {
        fprintf(stderr, "FAIL inter dct: cannot read packet sizes\n");
        goto done;
    }
    if (sizes[1] >= sizes[0] || sizes[2] >= sizes[0]) {
        fprintf(stderr, "FAIL inter dct: sizes %lu %lu %lu\n", sizes[0],
                sizes[1], sizes[2]);
        goto done;
    }
    movie = bink_open_file(path, BINK_MODE_DISK);
    if (movie == NULL) {
        fprintf(stderr, "FAIL inter dct: decoder rejected the file\n");
        goto done;
    }
    if (decode_within(movie, frames, N, W, H, y, u, v, 24, 4.0))
        rc = 0;
    bink_close(movie);
done:
    remove(path);
    free(frames);
    free(y);
    free(u);
    free(v);
    return rc;
}

static int test_inter_dct_no_motion_search(void)
{
    enum { W = 64, H = 48 };
    uint8_t *frames;
    mem_source mem;
    bink_enc_options opt;
    unsigned long sizes[8];
    char path[1024];
    char err[256];
    bink_enc_status status;
    uint8_t *y;
    uint8_t *u;
    uint8_t *v;
    size_t frame_bytes = (size_t)W * H * 3;
    size_t i;
    bink movie;
    int rc = 1;

    frames = (uint8_t *)calloc(2, frame_bytes);
    y = (uint8_t *)malloc((size_t)W * H);
    u = (uint8_t *)malloc((size_t)(W / 2) * (H / 2));
    v = (uint8_t *)malloc((size_t)(W / 2) * (H / 2));
    if (frames == NULL || y == NULL || u == NULL || v == NULL) {
        fprintf(stderr, "FAIL inter no-search: out of memory\n");
        goto done;
    }
    for (i = 0; i < (size_t)W * H; i++) {
        unsigned x = (unsigned)i % W;
        unsigned yy = (unsigned)i / W;
        uint8_t *p = frames + i * 3;

        /* Fine detail: intra coding is expensive here, so a small residual
         * is the only way the P-frame gets much smaller. */
        p[0] = (uint8_t)((x * 73 + yy * 151 + x * yy * 3) & 255);
        p[1] = (uint8_t)((x * 29 + yy * 47 + x * yy * 5) & 255);
        p[2] = (uint8_t)((x * 11 + yy * 97 + x * yy * 7) & 255);
    }
    /* The same picture plus a flat +3, so no exact match exists anywhere. */
    for (i = 0; i < frame_bytes; i++) {
        unsigned value = (unsigned)frames[i] + 3;

        frames[frame_bytes + i] = (uint8_t)(value > 255 ? 255 : value);
    }

    memset(&mem, 0, sizeof(mem));
    mem.frames = frames;
    mem.width = W;
    mem.height = H;
    mem.count = 2;
    mem.fail_index = -1;
    bink_enc_options_default(&opt);
    opt.verify = 1;
    opt.keyframe_interval = 2;
    opt.motion_range = 0;
    path_for(path, sizeof(path), "inter_no_search.bik");
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 0, err, sizeof(err));
    if (expect_ok(status, err, "inter no-search") != 0)
        goto done;
    if (packet_sizes_all(path, sizes, 8) != 2) {
        fprintf(stderr, "FAIL inter no-search: cannot read packet sizes\n");
        goto done;
    }
    if (sizes[1] * 4 >= sizes[0]) {
        fprintf(stderr, "FAIL inter no-search: sizes %lu %lu\n", sizes[0],
                sizes[1]);
        goto done;
    }
    movie = bink_open_file(path, BINK_MODE_DISK);
    if (movie == NULL) {
        fprintf(stderr, "FAIL inter no-search: decoder rejected the file\n");
        goto done;
    }
    if (decode_within(movie, frames, 2, W, H, y, u, v, 24, 4.0))
        rc = 0;
    bink_close(movie);
done:
    remove(path);
    free(frames);
    free(y);
    free(u);
    free(v);
    return rc;
}

static int test_inter_wrap_safety(void)
{
    enum { W = 64, H = 48 };
    uint8_t *frames;
    mem_source mem;
    bink_enc_options opt;
    unsigned long sizes[8];
    char path[1024];
    char err[256];
    bink_enc_status status;
    uint8_t *y;
    uint8_t *u;
    uint8_t *v;
    size_t frame_bytes = (size_t)W * H * 3;
    unsigned x;
    unsigned yy;
    bink movie;
    int rc = 1;

    frames = (uint8_t *)calloc(2, frame_bytes);
    y = (uint8_t *)malloc((size_t)W * H);
    u = (uint8_t *)malloc((size_t)(W / 2) * (H / 2));
    v = (uint8_t *)malloc((size_t)(W / 2) * (H / 2));
    if (frames == NULL || y == NULL || u == NULL || v == NULL) {
        fprintf(stderr, "FAIL inter wrap: out of memory\n");
        goto done;
    }
    for (yy = 0; yy < H; yy++) {
        for (x = 0; x < W; x++) {
            size_t p = ((size_t)yy * W + x) * 3;
            unsigned detail = (x * 7 + yy * 13) % 55;
            unsigned bright = 200 + detail;
            unsigned dark = 3 + detail;

            /* Detailed bright frame, then the same detail near black. The
             * residual is about -197, so a wrapped reconstruction lands back
             * near 250 and is far outside the block caps. */
            frames[p + 0] = (uint8_t)bright;
            frames[p + 1] = (uint8_t)bright;
            frames[p + 2] = (uint8_t)bright;
            frames[frame_bytes + p + 0] = (uint8_t)dark;
            frames[frame_bytes + p + 1] = (uint8_t)dark;
            frames[frame_bytes + p + 2] = (uint8_t)dark;
        }
    }

    memset(&mem, 0, sizeof(mem));
    mem.frames = frames;
    mem.width = W;
    mem.height = H;
    mem.count = 2;
    mem.fail_index = -1;
    bink_enc_options_default(&opt);
    opt.verify = 1;
    opt.keyframe_interval = 2;
    opt.motion_range = 0;
    path_for(path, sizeof(path), "inter_wrap.bik");
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 0, err, sizeof(err));
    if (expect_ok(status, err, "inter wrap") != 0)
        goto done;
    if (packet_sizes_all(path, sizes, 8) != 2 || sizes[1] * 4 >= sizes[0]) {
        fprintf(stderr, "FAIL inter wrap: sizes %lu %lu\n", sizes[0], sizes[1]);
        goto done;
    }
    movie = bink_open_file(path, BINK_MODE_DISK);
    if (movie == NULL) {
        fprintf(stderr, "FAIL inter wrap: decoder rejected the file\n");
        goto done;
    }
    if (decode_within(movie, frames, 2, W, H, y, u, v, 32, 5.0)) {
        const unsigned char *plane = bink_get_plane(movie, BINK_PLANE_Y);
        int stride = bink_get_stride(movie, 0);

        if (plane == NULL || stride <= 0 || plane[0] + 32 >= 200) {
            fprintf(stderr, "FAIL inter wrap: second frame stayed bright\n");
        } else {
            rc = 0;
        }
    }
    bink_close(movie);
done:
    remove(path);
    free(frames);
    free(y);
    free(u);
    free(v);
    return rc;
}

/* The inter DC bundle carries 10 bits plus a sign, so a residual whose mean
 * exceeds +/-127 cannot be coded and must be rejected, not truncated. */
static int test_inter_dc_range(void)
{
    uint8_t desired[64];
    uint8_t pred[64];
    int16_t t[64];
    int i;

    for (i = 0; i < 64; i++) {
        desired[i] = 250;
        pred[i] = 250;
    }
    if (bink_enc_video_score_inter(desired, pred, 0, t) != 0) {
        fprintf(stderr, "FAIL inter dc: flat residual rejected\n");
        return 1;
    }
    /* A -200 mean residual needs about -1600, well past the 10-bit range. */
    for (i = 0; i < 64; i++)
        pred[i] = 50;
    if (bink_enc_video_score_inter(desired, pred, 0, t) == 0) {
        fprintf(stderr, "FAIL inter dc: out-of-range residual accepted (t0=%d)\n",
                t[0]);
        return 1;
    }
    /* And a mild one inside the range must still pass. */
    for (i = 0; i < 64; i++)
        pred[i] = 247;
    if (bink_enc_video_score_inter(desired, pred, 0, t) != 0) {
        fprintf(stderr, "FAIL inter dc: mild residual rejected\n");
        return 1;
    }
    return 0;
}

/* Thirty chained P-frames: a drift between the encoder's predictor and the
 * decoder's reconstruction would show up as growing error or packet size. */
static int test_inter_chain(void)
{
    enum { W = 64, H = 48, N = 30 };
    uint8_t *frames;
    mem_source mem;
    bink_enc_options opt;
    unsigned long sizes[32];
    char path[1024];
    char err[256];
    bink_enc_status status;
    uint8_t *y;
    uint8_t *u;
    uint8_t *v;
    size_t frame_bytes = (size_t)W * H * 3;
    size_t i;
    int f;
    bink movie;
    int rc = 1;

    frames = (uint8_t *)calloc(N, frame_bytes);
    y = (uint8_t *)malloc((size_t)W * H);
    u = (uint8_t *)malloc((size_t)(W / 2) * (H / 2));
    v = (uint8_t *)malloc((size_t)(W / 2) * (H / 2));
    if (frames == NULL || y == NULL || u == NULL || v == NULL) {
        fprintf(stderr, "FAIL inter chain: out of memory\n");
        goto done;
    }
    for (i = 0; i < (size_t)W * H; i++) {
        unsigned px = (unsigned)i % W;
        unsigned py = (unsigned)i / W;
        uint8_t *p = frames + i * 3;

        p[0] = (uint8_t)((px * 73 + py * 151 + px * py * 3) & 255);
        p[1] = (uint8_t)((px * 29 + py * 47 + px * py * 5) & 255);
        p[2] = (uint8_t)((px * 11 + py * 97 + px * py * 7) & 255);
    }
    for (f = 1; f < N; f++) {
        uint8_t *dst = frames + (size_t)f * frame_bytes;

        memcpy(dst, frames, frame_bytes);
        for (i = 0; i < frame_bytes; i++) {
            unsigned value = (unsigned)dst[i] + (unsigned)f;

            dst[i] = (uint8_t)(value > 255 ? 255 : value);
        }
    }

    memset(&mem, 0, sizeof(mem));
    mem.frames = frames;
    mem.width = W;
    mem.height = H;
    mem.count = N;
    mem.fail_index = -1;
    bink_enc_options_default(&opt);
    opt.verify = 1;
    opt.keyframe_interval = 0;
    opt.motion_range = 4;
    path_for(path, sizeof(path), "inter_chain.bik");
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 0, err, sizeof(err));
    if (expect_ok(status, err, "inter chain") != 0)
        goto done;
    if (packet_sizes_all(path, sizes, 32) != N) {
        fprintf(stderr, "FAIL inter chain: cannot read packet sizes\n");
        goto done;
    }
    if (sizes[N - 1] > sizes[1] * 2) {
        fprintf(stderr, "FAIL inter chain: last packet %lu, first P %lu\n",
                sizes[N - 1], sizes[1]);
        goto done;
    }
    movie = bink_open_file(path, BINK_MODE_DISK);
    if (movie == NULL) {
        fprintf(stderr, "FAIL inter chain: decoder rejected the file\n");
        goto done;
    }
    if (decode_within(movie, frames, N, W, H, y, u, v, 24, 4.0))
        rc = 0;
    bink_close(movie);
done:
    remove(path);
    free(frames);
    free(y);
    free(u);
    free(v);
    return rc;
}

static int test_dct_roundtrip(void)
{
    /* gradient, hard edge, flat, and pseudo-random blocks */
    const int kinds = 4;
    int kind;
    int failures = 0;

    for (kind = 0; kind < kinds; kind++) {
        uint8_t pixels[64];
        double coef[64];
        int16_t t[64];
        int32_t recon[64];
        uint8_t out[64];
        const int32_t *quant = bink_enc_intra_quant(0);
        int i;
        int maxerr = 0;

        for (i = 0; i < 64; i++) {
            int x = i & 7;
            int y = i >> 3;

            if (kind == 0)
                pixels[i] = (uint8_t)(20 + x * 25 + y * 2);
            else if (kind == 1)
                pixels[i] = x < 4 ? 230 : 20;
            else if (kind == 2)
                pixels[i] = 123;
            else
                pixels[i] = (uint8_t)((i * 37 + 11) & 255);
        }
        bink_enc_fdct(pixels, coef);
        if (bink_enc_quantize(coef, quant, t) != 0)
            return 1;
        bink_enc_unquantize(t, quant, recon);
        bink_idct_put(out, 8, recon);
        for (i = 0; i < 64; i++) {
            int e = out[i] - (int)pixels[i];
            if (e < 0)
                e = -e;
            if (e > maxerr)
                maxerr = e;
        }
        if (maxerr > 2)
            failures++;
    }
    return failures;
}

/* Write t, count it with a NULL writer, and read it back with the decoder's
 * parser. Returns 0 when every step agrees. */
static int coeff_writer_trial(const int16_t t[64], int quant_index)
{
    BinkBitWriter bw;
    BinkBits bits;
    int16_t back[64];
    int cbits = bink_enc_cbits(t);
    int bits_written;
    int bits_counted;
    int q;

    bink_bw_init(&bw);
    bits_written = bink_enc_emit_coeffs(&bw, t, cbits, quant_index);
    if (bits_written < 0) {
        bink_bw_free(&bw);
        return 1;
    }
    bits_counted = bink_enc_emit_coeffs(NULL, t, cbits, quant_index);
    if (bits_counted != bits_written) {
        bink_bw_free(&bw);
        return 1;
    }
    bink_bits_init(&bits, bink_bw_data(&bw),
                   (size_t)((bits_written + 7) / 8));
    q = bink_video_read_coeffs(&bits, back);
    bink_bw_free(&bw);
    if (q != quant_index)
        return 1;
    if (memcmp(t, back, sizeof(back)) != 0)
        return 1;
    return 0;
}

static int test_coeff_writer_roundtrip(void)
{
    int16_t t[64];
    uint32_t seed = 12345u;
    int trial;

    for (trial = 0; trial < 64; trial++) {
        int i;

        memset(t, 0, sizeof(t));
        /* sparse AC values; DC is bundled and must stay 0 here */
        for (i = 1; i < 64; i++) {
            seed = seed * 1103515245u + 12345u;
            if (((seed >> 16) & 3) == 0)
                t[i] = (int16_t)((int)((seed >> 20) % 2047) - 1023);
        }
        if (trial == 0)
            memset(t, 0, sizeof(t)); /* all-zero AC must round-trip too */

        if (coeff_writer_trial(t, trial & 15) != 0)
            return 1;
    }
    return 0;
}

static int test_coeff_writer_high_planes(void)
{
    int16_t t[64];
    uint32_t seed = 2463534242u;
    int i;

    memset(t, 0, sizeof(t));
    /* Dense AC with magnitudes up to 32767 so planes 10..14 are coded. */
    for (i = 1; i < 64; i++) {
        int magnitude;

        seed = seed * 1103515245u + 12345u;
        magnitude = (int)((seed >> 12) % 32767) + 1;
        t[i] = (int16_t)((seed & 1) ? -magnitude : magnitude);
    }
    t[7] = 32767;
    t[13] = -32767;
    if (bink_enc_cbits(t) != 15)
        return 1;
    return coeff_writer_trial(t, 9);
}

static int test_cbits(void)
{
    int16_t t[64];

    memset(t, 0, sizeof(t));
    if (bink_enc_cbits(t) != 0)
        return 1;
    t[5] = 1;
    if (bink_enc_cbits(t) != 1)
        return 1;
    t[5] = -1023;
    if (bink_enc_cbits(t) != 10)
        return 1;
    t[5] = 1024;
    if (bink_enc_cbits(t) != 11)
        return 1;
    return 0;
}

static int test_quantize_rejects_out_of_range(void)
{
    double coef[64];
    int16_t t[64];
    const int32_t *quant = bink_enc_intra_quant(0);
    int i;

    memset(coef, 0, sizeof(coef));
    for (i = 0; i < 64; i++)
        coef[i] = 1.0e7;
    return bink_enc_quantize(coef, quant, t) == 0 ? 1 : 0;
}

static int check_audio_samples(const char *path, unsigned frames,
                               int expect_zero, int expect_value)
{
    bink movie = bink_open_file(path, BINK_MODE_DISK);
    unsigned long got_frames = 0;
    unsigned frame;

    if (movie == NULL) {
        fprintf(stderr, "FAIL audio open %s\n", path);
        return 1;
    }
    CHECK(bink_info_all(movie, NULL, &got_frames, NULL) == 0);
    CHECK(got_frames == frames);
    CHECK(bink_enable_video(movie, 1) == 0);
    CHECK(bink_enable_audio(movie, 0, 1) == 0);
    {
        char status = bink_first(movie);

        for (frame = 0; frame < frames; frame++) {
            const unsigned char *pcm;
            unsigned long size;
            unsigned long i;
            long sum = 0;
            int samples;
            int min_s = 0;
            int max_s = 0;

            CHECK(status == BINK_MORE || status == BINK_LAST);
            pcm = bink_get_audio(movie, 0);
            size = bink_get_audio_size(movie, 0);
            CHECK(pcm != NULL && size >= 2 && (size % 2) == 0);
            samples = (int)(size / 2);
            for (i = 0; i < size; i += 2) {
                int sample = (int)((const int16_t *)(const void *)pcm)[i / 2];

                if (i == 0 || sample < min_s)
                    min_s = sample;
                if (i == 0 || sample > max_s)
                    max_s = sample;
                sum += sample;
                if (expect_zero)
                    CHECK(sample == 0);
            }
            if (!expect_zero) {
                long mean = sum / samples;

                if (mean < expect_value - 50 || mean > expect_value + 50 ||
                    max_s - min_s > 100) {
                    fprintf(stderr,
                            "FAIL constant audio frame %u mean %ld min %d max %d\n",
                            frame, mean, min_s, max_s);
                    bink_close(movie);
                    return 1;
                }
            }
            if (frame + 1 < frames)
                status = bink_next(movie);
        }
    }
    bink_close(movie);
    return 0;
}

/* Decode every frame's audio into one contiguous buffer. */
static int collect_audio(const char *path, int16_t **samples, size_t *count,
                         int *channels)
{
    bink movie = bink_open_file(path, BINK_MODE_DISK);
    unsigned long frames = 0;
    unsigned char mask = 0;
    unsigned char chans[7];
    unsigned char depth[7];
    unsigned long rates[7];
    int16_t *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    char status;
    unsigned long f;
    int rc = 1;

    if (movie == NULL) {
        fprintf(stderr, "FAIL audio: cannot open %s\n", path);
        return 1;
    }
    if (bink_info_all(movie, NULL, &frames, NULL) != 0 ||
        bink_info_audio(movie, &mask, chans, depth, rates) != 0 ||
        (mask & 1) == 0 || bink_enable_video(movie, 1) != 0 ||
        bink_enable_audio(movie, 0, 1) != 0) {
        fprintf(stderr, "FAIL audio: cannot query %s\n", path);
        goto done;
    }
    status = bink_first(movie);
    for (f = 0; f < frames; f++) {
        const unsigned char *pcm;
        unsigned long size;

        if (status != BINK_MORE && status != BINK_LAST) {
            fprintf(stderr, "FAIL audio: stopped at frame %lu\n", f);
            goto done;
        }
        pcm = bink_get_audio(movie, 0);
        size = bink_get_audio_size(movie, 0);
        if (pcm == NULL || size == 0) {
            fprintf(stderr, "FAIL audio: no samples at frame %lu\n", f);
            goto done;
        }
        if (len + size / 2 > cap) {
            size_t want = (len + size / 2) * 2 + 4096;
            int16_t *grown = (int16_t *)realloc(buf, want * sizeof(*buf));

            if (grown == NULL) {
                fprintf(stderr, "FAIL audio: out of memory\n");
                goto done;
            }
            buf = grown;
            cap = want;
        }
        memcpy(buf + len, pcm, size);
        len += size / 2;
        if (f + 1 < frames)
            status = bink_next(movie);
    }
    *samples = buf;
    *count = len;
    *channels = chans[0];
    buf = NULL;
    rc = 0;
done:
    free(buf);
    bink_close(movie);
    return rc;
}

static int test_audio_sine_frequency(void)
{
    enum { RATE = 44100, FREQ = 440, AMPLITUDE = 12000, VIDEO_FRAMES = 15 };
    enum { W = 8, H = 8 };
    uint8_t *frames;
    int16_t *pcm;
    mem_source mem;
    bink_enc_options opt;
    char path[1024];
    char err[256];
    bink_enc_status status;
    int16_t *decoded = NULL;
    size_t decoded_count = 0;
    int channels = 0;
    unsigned i;
    size_t n;
    double crossings = 0.0;
    double expected;
    double sum = 0.0;
    int rc = 1;

    frames = (uint8_t *)calloc(VIDEO_FRAMES, (size_t)W * H * 3);
    pcm = (int16_t *)malloc((size_t)RATE * sizeof(*pcm));
    if (frames == NULL || pcm == NULL) {
        fprintf(stderr, "FAIL audio sine: out of memory\n");
        goto done;
    }
    for (i = 0; i < VIDEO_FRAMES; i++)
        fill_rgb(frames + (size_t)i * W * H * 3, W, H, 10, 20, 30);
    for (i = 0; i < RATE; i++)
        pcm[i] = (int16_t)lrint(AMPLITUDE * sin(2.0 * M_PI * FREQ * (double)i /
                                               (double)RATE));

    memset(&mem, 0, sizeof(mem));
    mem.frames = frames;
    mem.width = W;
    mem.height = H;
    mem.count = VIDEO_FRAMES;
    mem.channels = 1;
    mem.pcm = pcm;
    mem.sample_count = RATE;
    mem.fail_index = -1;
    bink_enc_options_default(&opt);
    opt.verify = 1;
    opt.motion_range = 0;
    path_for(path, sizeof(path), "sine.bik");
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, RATE, err, sizeof(err));
    if (expect_ok(status, err, "audio sine") != 0)
        goto done;
    if (collect_audio(path, &decoded, &decoded_count, &channels) != 0)
        goto done;
    if (channels != 1 || decoded_count < (size_t)RATE / 2) {
        fprintf(stderr, "FAIL audio sine: decoded %zu samples, %d channels\n",
                decoded_count, channels);
        goto done;
    }
    for (n = 0; n < decoded_count; n++) {
        sum += (double)decoded[n] * (double)decoded[n];
        if (n > 0 && ((decoded[n - 1] < 0) != (decoded[n] < 0)))
            crossings += 1.0;
    }
    if (sqrt(sum / (double)decoded_count) < 1000.0) {
        fprintf(stderr, "FAIL audio sine: RMS too low\n");
        goto done;
    }
    expected = 2.0 * (double)FREQ * (double)decoded_count / (double)RATE;
    if (fabs(crossings - expected) > expected * 0.01) {
        fprintf(stderr, "FAIL audio sine: %.1f crossings, expected %.1f\n",
                crossings, expected);
        goto done;
    }
    /* Past the source, only the last block's overlap tail remains. It must be
     * silence plus ringing, never a reused copy of earlier samples. */
    if (decoded_count > (size_t)RATE) {
        for (n = (size_t)RATE; n < decoded_count; n++) {
            int magnitude = decoded[n] < 0 ? -decoded[n] : decoded[n];

            if (magnitude > 1500) {
                fprintf(stderr,
                        "FAIL audio sine: tail sample %zu is %d after the source\n",
                        n - (size_t)RATE, decoded[n]);
                goto done;
            }
        }
    }
    rc = 0;
done:
    remove(path);
    free(frames);
    free(pcm);
    free(decoded);
    return rc;
}

/* A short video clip carrying a sine (or two, for stereo). */
typedef struct {
    uint8_t *frames;
    int16_t *pcm;
    size_t samples;
    mem_source mem;
} sine_clip;

static void free_sine(sine_clip *clip)
{
    free(clip->frames);
    free(clip->pcm);
    memset(clip, 0, sizeof(*clip));
}

static int make_sine(sine_clip *clip, unsigned rate, unsigned channels,
                     double freq_l, double freq_r, unsigned seconds)
{
    enum { W = 8, H = 8 };
    size_t per_frame = (size_t)W * H * 3;
    unsigned nframes = seconds * 15;
    size_t n = (size_t)rate * seconds;
    size_t i;

    memset(clip, 0, sizeof(*clip));
    clip->frames = (uint8_t *)calloc(nframes, per_frame);
    clip->pcm = (int16_t *)malloc(n * channels * sizeof(int16_t));
    if (clip->frames == NULL || clip->pcm == NULL) {
        free_sine(clip);
        return -1;
    }
    for (i = 0; i < nframes; i++)
        fill_rgb(clip->frames + i * per_frame, W, H, 10, 20, 30);
    for (i = 0; i < n; i++) {
        clip->pcm[i * channels] =
            (int16_t)lrint(12000.0 * sin(2.0 * M_PI * freq_l * (double)i /
                                         (double)rate));
        if (channels == 2)
            clip->pcm[i * channels + 1] =
                (int16_t)lrint(12000.0 * sin(2.0 * M_PI * freq_r * (double)i /
                                             (double)rate));
    }
    clip->samples = n;
    clip->mem.frames = clip->frames;
    clip->mem.width = W;
    clip->mem.height = H;
    clip->mem.count = nframes;
    clip->mem.channels = channels;
    clip->mem.pcm = clip->pcm;
    clip->mem.sample_count = n;
    clip->mem.fail_index = -1;
    return 0;
}

static bink_enc_status encode_sine(sine_clip *clip, const char *name,
                                   unsigned rate, char *err, size_t errlen)
{
    bink_enc_options opt;
    char path[1024];

    bink_enc_options_default(&opt);
    opt.verify = 1;
    opt.motion_range = 0;
    path_for(path, sizeof(path), name);
    err[0] = '\0';
    return encode_mem(&clip->mem, &opt, path, rate, err, errlen);
}

static double decoded_snr(const int16_t *decoded, size_t decoded_count,
                          const int16_t *source, size_t source_count,
                          unsigned channels)
{
    double signal = 0.0;
    double noise = 0.0;
    size_t n;
    size_t limit = decoded_count < source_count ? decoded_count : source_count;

    for (n = 0; n < limit; n++) {
        size_t c;

        for (c = 0; c < channels; c++) {
            double s = (double)source[n * channels + c];
            double d = (double)decoded[n * channels + c] - s;

            signal += s * s;
            noise += d * d;
        }
    }
    if (noise <= 0.0)
        return 999.0;
    if (signal <= 0.0)
        return -999.0;
    return 10.0 * log10(signal / noise);
}

static double count_crossings(const int16_t *pcm, size_t first, size_t count,
                              unsigned stride, unsigned offset)
{
    double crossings = 0.0;
    size_t n;

    for (n = first + 1; n < count; n++) {
        int a = pcm[(n - 1) * stride + offset];
        int b = pcm[n * stride + offset];

        if ((a < 0) != (b < 0))
            crossings += 1.0;
    }
    return crossings;
}

/* Silence is the case zero-run coding exists for: without it every block
 * still pays a chunk header per eight zero coefficients. */
static int test_audio_silence_rle(void)
{
    enum { RATE = 44100 };
    sine_clip clip;
    char path[1024];
    char err[256];
    bink_enc_status status;
    struct stat st;
    int rc = 1;

    if (make_sine(&clip, RATE, 1, 440.0, 0.0, 1) != 0) {
        fprintf(stderr, "FAIL audio silence: out of memory\n");
        return 1;
    }
    memset(clip.pcm, 0, clip.samples * sizeof(int16_t));
    path_for(path, sizeof(path), "silence_rle.bik");
    status = encode_sine(&clip, "silence_rle.bik", RATE, err, sizeof(err));
    if (expect_ok(status, err, "audio silence") != 0)
        goto done;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "FAIL audio silence: no output file\n");
        goto done;
    }
    /* The 15 small video frames cost about 1.3 kB on their own; without
     * zero-run coding the silent audio adds another 4.6 kB. */
    if (st.st_size >= 3200) {
        fprintf(stderr, "FAIL audio silence: %ld bytes\n", (long)st.st_size);
        goto done;
    }
    rc = 0;
done:
    remove(path);
    free_sine(&clip);
    return rc;
}

static int test_audio_sine_snr(void)
{
    enum { RATE = 44100, FREQ = 440 };
    sine_clip clip;
    char path[1024];
    char err[256];
    bink_enc_status status;
    int16_t *decoded = NULL;
    size_t decoded_count = 0;
    int channels = 0;
    double snr;
    int rc = 1;

    if (make_sine(&clip, RATE, 1, FREQ, 0.0, 1) != 0) {
        fprintf(stderr, "FAIL audio snr: out of memory\n");
        return 1;
    }
    path_for(path, sizeof(path), "sine_snr.bik");
    status = encode_sine(&clip, "sine_snr.bik", RATE, err, sizeof(err));
    if (expect_ok(status, err, "audio snr") != 0)
        goto done;
    if (collect_audio(path, &decoded, &decoded_count, &channels) != 0)
        goto done;
    if (channels != 1) {
        fprintf(stderr, "FAIL audio snr: %d channels\n", channels);
        goto done;
    }
    snr = decoded_snr(decoded, decoded_count, clip.pcm, clip.samples, 1);
    if (snr < 20.0) {
        fprintf(stderr, "FAIL audio snr: %.2f dB\n", snr);
        goto done;
    }
    rc = 0;
done:
    remove(path);
    free(decoded);
    free_sine(&clip);
    return rc;
}

static int test_audio_size(void)
{
    enum { RATE = 44100, FREQ = 440 };
    sine_clip clip;
    char path[1024];
    char err[256];
    bink_enc_status status;
    struct stat st;
    int rc = 1;

    if (make_sine(&clip, RATE, 1, FREQ, 0.0, 1) != 0) {
        fprintf(stderr, "FAIL audio size: out of memory\n");
        return 1;
    }
    path_for(path, sizeof(path), "sine_size.bik");
    status = encode_sine(&clip, "sine_size.bik", RATE, err, sizeof(err));
    if (expect_ok(status, err, "audio size") != 0)
        goto done;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "FAIL audio size: no output file\n");
        goto done;
    }
    /* A quarter of the raw PCM: the per-band target must keep a tone's
     * spectral leakage from dominating the file. */
    if (st.st_size >= (off_t)(clip.samples * 2 / 4)) {
        fprintf(stderr, "FAIL audio size: %ld bytes for %zu samples\n",
                (long)st.st_size, clip.samples);
        goto done;
    }
    rc = 0;
done:
    remove(path);
    free_sine(&clip);
    return rc;
}

/* Magnitude of one frequency in a PCM stream, Goertzel style. */
static double tone_magnitude(const int16_t *pcm, size_t count,
                             unsigned stride, unsigned offset, unsigned rate,
                             double freq, size_t skip)
{
    double re = 0.0;
    double im = 0.0;
    size_t n;

    for (n = skip; n < count; n++) {
        double angle = 2.0 * M_PI * freq * (double)n / (double)rate;
        double s = (double)pcm[n * stride + offset];

        re += s * cos(angle);
        im += s * sin(angle);
    }
    return sqrt(re * re + im * im) / (double)(count - skip);
}

/* A wide dynamic range is what per-band quantizers buy: one global step sized
 * for the loudest partial turns the quiet ones into a single count. */
static int test_audio_quiet_bands(void)
{
    enum { RATE = 44100, TONES = 7 };
    static const double freqs[TONES] = { 200,  800,  2000, 4000,
                                         6500, 9500, 13000 };
    static const double amps[TONES] = { 12000, 3000, 800, 200, 60, 18, 5 };
    sine_clip clip;
    char path[1024];
    char err[256];
    bink_enc_status status;
    int16_t *decoded = NULL;
    size_t decoded_count = 0;
    int channels = 0;
    size_t i;
    int j;
    int rc = 1;

    if (make_sine(&clip, RATE, 1, 1000.0, 0.0, 1) != 0) {
        fprintf(stderr, "FAIL audio bands: out of memory\n");
        return 1;
    }
    for (i = 0; i < clip.samples; i++) {
        double sum = 0.0;

        for (j = 0; j < TONES; j++)
            sum += amps[j] * sin(2.0 * M_PI * freqs[j] * (double)i /
                                 (double)RATE);
        clip.pcm[i] = (int16_t)lrint(sum);
    }
    path_for(path, sizeof(path), "quiet_bands.bik");
    status = encode_sine(&clip, "quiet_bands.bik", RATE, err, sizeof(err));
    if (expect_ok(status, err, "audio bands") != 0)
        goto done;
    if (collect_audio(path, &decoded, &decoded_count, &channels) != 0)
        goto done;
    if (channels != 1 || decoded_count < (size_t)RATE / 2) {
        fprintf(stderr, "FAIL audio bands: %zu samples\n", decoded_count);
        goto done;
    }
    for (j = 0; j < TONES; j++) {
        double source = tone_magnitude(clip.pcm, clip.samples, 1, 0, RATE,
                                       freqs[j], 2048);
        double got = tone_magnitude(decoded, decoded_count, 1, 0, RATE,
                                    freqs[j], 2048);

        if (source < 1.0)
            continue;
        if (got < source * 0.6) {
            fprintf(stderr,
                    "FAIL audio bands: %.0f Hz survived at %.1f of %.1f\n",
                    freqs[j], got, source);
            goto done;
        }
    }
    rc = 0;
done:
    remove(path);
    free(decoded);
    free_sine(&clip);
    return rc;
}

static int test_audio_stereo_channels(void)
{
    enum { RATE = 44100, LEFT = 440, RIGHT = 660 };
    sine_clip clip;
    char path[1024];
    char err[256];
    bink_enc_status status;
    int16_t *decoded = NULL;
    size_t decoded_count = 0;
    int channels = 0;
    double left;
    double right;
    double expected_left;
    double expected_right;
    int rc = 1;

    if (make_sine(&clip, RATE, 2, LEFT, RIGHT, 1) != 0) {
        fprintf(stderr, "FAIL audio stereo: out of memory\n");
        return 1;
    }
    path_for(path, sizeof(path), "stereo_channels.bik");
    status = encode_sine(&clip, "stereo_channels.bik", RATE, err,
                         sizeof(err));
    if (expect_ok(status, err, "audio stereo") != 0)
        goto done;
    if (collect_audio(path, &decoded, &decoded_count, &channels) != 0)
        goto done;
    if (channels != 2) {
        fprintf(stderr, "FAIL audio stereo: %d channels\n", channels);
        goto done;
    }
    left = count_crossings(decoded, 0, decoded_count / 2, 2, 0);
    right = count_crossings(decoded, 0, decoded_count / 2, 2, 1);
    expected_left = 2.0 * LEFT * (double)decoded_count / 2.0 / (double)RATE;
    expected_right = 2.0 * RIGHT * (double)decoded_count / 2.0 / (double)RATE;
    if (fabs(left - expected_left) > expected_left * 0.01 ||
        fabs(right - expected_right) > expected_right * 0.01 ||
        fabs(left - right) < 50.0) {
        fprintf(stderr, "FAIL audio stereo: %.0f/%.0f crossings, expected %.0f/%.0f\n",
                left, right, expected_left, expected_right);
        goto done;
    }
    rc = 0;
done:
    remove(path);
    free(decoded);
    free_sine(&clip);
    return rc;
}

static int test_audio_rates(void)
{
    static const unsigned rates[2] = { 22050, 48000 };
    enum { FREQ = 440 };
    int r;
    int rc = 0;

    for (r = 0; r < 2; r++) {
        sine_clip clip;
        char path[1024];
        char err[256];
        bink_enc_status status;
        int16_t *decoded = NULL;
        size_t decoded_count = 0;
        int channels = 0;
        double crossings;
        double estimate;
        char name[64];

        if (make_sine(&clip, rates[r], 1, FREQ, 0.0, 1) != 0) {
            fprintf(stderr, "FAIL audio rates: out of memory\n");
            return 1;
        }
        snprintf(name, sizeof(name), "rate_%u.bik", rates[r]);
        path_for(path, sizeof(path), name);
        status = encode_sine(&clip, name, rates[r], err, sizeof(err));
        if (expect_ok(status, err, "audio rates") != 0) {
            free_sine(&clip);
            return 1;
        }
        if (collect_audio(path, &decoded, &decoded_count, &channels) != 0) {
            free_sine(&clip);
            return 1;
        }
        crossings = count_crossings(decoded, 0, decoded_count, 1, 0);
        estimate = crossings * (double)rates[r] / (2.0 * (double)decoded_count);
        if (fabs(estimate - (double)FREQ) > (double)FREQ * 0.02) {
            fprintf(stderr, "FAIL audio rates %u: %.1f Hz\n", rates[r],
                    estimate);
            rc = 1;
        }
        free(decoded);
        remove(path);
        free_sine(&clip);
        if (rc != 0)
            break;
    }
    return rc;
}

/* The FFT-based forward transform against the definition it implements. */
static int test_audio_dct_forward(void)
{
    static const int lens[3] = { 512, 1024, 2048 };
    int li;

    for (li = 0; li < 3; li++) {
        int n = lens[li];
        BinkAudioDct *dct = bink_audio_dct_open(n);
        float *src = (float *)malloc((size_t)n * sizeof(*src));
        double *got = (double *)malloc((size_t)n * sizeof(*got));
        uint32_t seed = 2463534242u;
        double worst = 0.0;
        int k;
        int t;

        if (dct == NULL || src == NULL || got == NULL) {
            fprintf(stderr, "FAIL audio dct: out of memory\n");
            bink_audio_dct_close(dct);
            free(src);
            free(got);
            return 1;
        }
        for (t = 0; t < n; t++) {
            seed = seed * 1103515245u + 12345u;
            src[t] = (float)((int)((seed >> 16) % 2001u) - 1000) / 1000.0f;
        }
        if (bink_audio_dct_forward(dct, src, got) != 0) {
            fprintf(stderr, "FAIL audio dct: forward failed at %d\n", n);
            bink_audio_dct_close(dct);
            free(src);
            free(got);
            return 1;
        }
        for (k = 0; k < n; k++) {
            double want = 0.0;
            double diff;

            for (t = 0; t < n; t++)
                want += (double)src[t] * cos(M_PI / (double)n *
                                             ((double)t + 0.5) * (double)k);
            diff = fabs(got[k] - want);
            if (diff > worst)
                worst = diff;
        }
        bink_audio_dct_close(dct);
        free(src);
        free(got);
        if (worst > 1.0e-6) {
            fprintf(stderr, "FAIL audio dct: n=%d worst %.3g\n", n, worst);
            return 1;
        }
    }
    return 0;
}

static int test_audio(void)
{
    enum { W = 8, H = 8 };
    uint8_t frames[2 * W * H * 3];
    int16_t *tone;
    mem_source mem;
    bink_enc_options opt;
    char path[1024];
    char err[256];
    bink_enc_status status;
    unsigned i;

    memset(frames, 0, sizeof(frames));
    fill_rgb(frames, W, H, 10, 20, 30);
    fill_rgb(frames + W * H * 3, W, H, 10, 20, 30);
    memset(&mem, 0, sizeof(mem));
    mem.frames = frames;
    mem.width = W;
    mem.height = H;
    mem.count = 2;
    mem.channels = 1;
    mem.sample_count = 0;
    mem.pcm = NULL;
    mem.fail_index = -1;
    bink_enc_options_default(&opt);
    opt.verify = 1;
    opt.motion_range = 0;
    path_for(path, sizeof(path), "silence.bik");
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 22050, err, sizeof(err));
    if (expect_ok(status, err, "silence") != 0)
        return 1;
    if (check_audio_samples(path, 2, 1, 0) != 0) {
        remove(path);
        return 1;
    }
    remove(path);

    tone = (int16_t *)malloc(1920 * sizeof(int16_t));
    if (tone == NULL)
        return 1;
    for (i = 0; i < 1920; i++)
        tone[i] = 10000;
    mem.count = 1;
    mem.pcm = tone;
    mem.sample_count = 1920;
    path_for(path, sizeof(path), "tone.bik");
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 22050, err, sizeof(err));
    free(tone);
    if (expect_ok(status, err, "tone") != 0)
        return 1;
    if (check_audio_samples(path, 1, 0, 10000) != 0) {
        remove(path);
        return 1;
    }
    remove(path);

    mem.channels = 2;
    mem.pcm = NULL;
    mem.sample_count = 0;
    path_for(path, sizeof(path), "stereo.bik");
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 22050, err, sizeof(err));
    if (expect_ok(status, err, "stereo") != 0)
        return 1;
    if (check_audio_samples(path, 1, 1, 0) != 0) {
        remove(path);
        return 1;
    }
    remove(path);
    return 0;
}

static int test_failure_keeps_or_removes(void)
{
    uint8_t rgb[8 * 8 * 3];
    mem_source mem;
    bink_enc_options opt;
    char path[1024];
    char err[256];
    FILE *fp;
    bink_enc_status status;

    fill_rgb(rgb, 8, 8, 1, 2, 3);
    memset(&mem, 0, sizeof(mem));
    mem.frames = rgb;
    mem.width = 8;
    mem.height = 8;
    mem.count = 1;
    mem.fail_index = -1;
    bink_enc_options_default(&opt);
    opt.motion_range = 16;
    path_for(path, sizeof(path), "sentinel.bik");
    fp = fopen(path, "wb");
    CHECK(fp != NULL);
    CHECK(fwrite("keep", 1, 4, fp) == 4);
    fclose(fp);
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 0, err, sizeof(err));
    CHECK(status == BINK_ENC_ERR_INPUT);
    fp = fopen(path, "rb");
    CHECK(fp != NULL);
    {
        char buf[4];

        CHECK(fread(buf, 1, 4, fp) == 4);
        fclose(fp);
        CHECK(memcmp(buf, "keep", 4) == 0);
    }
    remove(path);

    opt.motion_range = 0;
    mem.fail_index = 0;
    path_for(path, sizeof(path), "partial.bik");
    err[0] = '\0';
    status = encode_mem(&mem, &opt, path, 0, err, sizeof(err));
    CHECK(status == BINK_ENC_ERR_INPUT);
    fp = fopen(path, "rb");
    CHECK(fp == NULL);
    return 0;
}

static void put_u16(unsigned char *p, unsigned value)
{
    p[0] = (unsigned char)(value & 255u);
    p[1] = (unsigned char)((value >> 8) & 255u);
}

static void put_u32(unsigned char *p, unsigned value)
{
    p[0] = (unsigned char)(value & 255u);
    p[1] = (unsigned char)((value >> 8) & 255u);
    p[2] = (unsigned char)((value >> 16) & 255u);
    p[3] = (unsigned char)((value >> 24) & 255u);
}

static int write_bytes(const char *path, const unsigned char *data, size_t size)
{
    FILE *fp = fopen(path, "wb");

    if (fp == NULL)
        return -1;
    if (fwrite(data, 1, size, fp) != size) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int write_ppm(const char *path, const uint8_t *rgb, unsigned width,
                     unsigned height)
{
    FILE *fp = fopen(path, "wb");
    size_t bytes = (size_t)width * height * 3;

    if (fp == NULL)
        return -1;
    if (fprintf(fp, "P6\n# comment\n%u %u\n255\n", width, height) < 0) {
        fclose(fp);
        return -1;
    }
    if (fwrite(rgb, 1, bytes, fp) != bytes) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int write_bmp(const char *path, const uint8_t *rgb, unsigned width,
                     unsigned height, int bpp, int alpha, int top_down)
{
    unsigned channels = (unsigned)bpp / 8;
    unsigned row_bytes = ((width * channels + 3u) / 4u) * 4u;
    unsigned pixels = row_bytes * height;
    unsigned char header[54];
    unsigned char *row;
    FILE *fp;
    unsigned y;
    int height_field = top_down ? -(int)height : (int)height;

    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    put_u32(header + 2, 54u + pixels);
    put_u32(header + 10, 54);
    put_u32(header + 14, 40);
    put_u32(header + 18, width);
    put_u32(header + 22, (unsigned)height_field);
    put_u16(header + 26, 1);
    put_u16(header + 28, (unsigned)bpp);
    row = (unsigned char *)calloc(1, row_bytes);
    fp = fopen(path, "wb");
    if (row == NULL || fp == NULL || fwrite(header, 1, 54, fp) != 54) {
        free(row);
        if (fp != NULL)
            fclose(fp);
        return -1;
    }
    for (y = 0; y < height; y++) {
        unsigned x;

        memset(row, 0, row_bytes);
        for (x = 0; x < width; x++) {
            unsigned src_y = top_down ? y : height - 1 - y;
            unsigned char *dst = row + x * channels;
            const uint8_t *src = rgb + ((size_t)src_y * width + x) * 3;

            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            if (channels == 4)
                dst[3] = (unsigned char)alpha;
        }
        if (fwrite(row, 1, row_bytes, fp) != row_bytes) {
            free(row);
            fclose(fp);
            return -1;
        }
    }
    free(row);
    fclose(fp);
    return 0;
}

static int write_wav(const char *path, unsigned rate, unsigned channels,
                     unsigned bits, const unsigned char *samples,
                     unsigned frames)
{
    unsigned block = channels * (bits / 8);
    unsigned data_bytes = frames * block;
    unsigned char header[44];
    FILE *fp;

    memset(header, 0, sizeof(header));
    memcpy(header, "RIFF", 4);
    put_u32(header + 4, 36u + data_bytes);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    put_u32(header + 16, 16);
    put_u16(header + 20, 1);
    put_u16(header + 22, channels);
    put_u32(header + 24, rate);
    put_u32(header + 28, rate * block);
    put_u16(header + 32, block);
    put_u16(header + 34, bits);
    memcpy(header + 36, "data", 4);
    put_u32(header + 40, data_bytes);
    fp = fopen(path, "wb");
    if (fp == NULL)
        return -1;
    if (fwrite(header, 1, 44, fp) != 44 ||
        fwrite(samples, 1, data_bytes, fp) != data_bytes) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int test_image_and_wav_load(void)
{
    uint8_t rgb[2 * 2 * 3] = {
        10, 20, 30, 40, 50, 60,
        70, 80, 90, 100, 110, 120
    };
    unsigned char wav24[6] = { 0x80, 0x01, 0x01, 0x00, 0x00, 0xff };
    unsigned char wav8[1] = { 255 };
    unsigned char wav16[2] = { 0xe8, 0x03 };
    uint8_t *loaded = NULL;
    unsigned width = 0;
    unsigned height = 0;
    char path[1024];
    char err[256];
    bink_pcm_audio audio;

    path_for(path, sizeof(path), "tiny.ppm");
    CHECK(write_ppm(path, rgb, 2, 2) == 0);
    err[0] = '\0';
    CHECK(bink_enc_image_load(path, &loaded, &width, &height, err,
                              sizeof(err)) == BINK_ENC_OK);
    CHECK(width == 2 && height == 2);
    CHECK(memcmp(loaded, rgb, sizeof(rgb)) == 0);
    free(loaded);
    loaded = NULL;
    remove(path);

    path_for(path, sizeof(path), "tiny24.bmp");
    CHECK(write_bmp(path, rgb, 2, 2, 24, 0, 0) == 0);
    CHECK(bink_enc_image_load(path, &loaded, &width, &height, err,
                              sizeof(err)) == BINK_ENC_OK);
    CHECK(width == 2 && height == 2);
    CHECK(memcmp(loaded, rgb, sizeof(rgb)) == 0);
    free(loaded);
    loaded = NULL;
    remove(path);

    path_for(path, sizeof(path), "tiny24top.bmp");
    CHECK(write_bmp(path, rgb, 2, 2, 24, 0, 1) == 0);
    CHECK(bink_enc_image_load(path, &loaded, &width, &height, err,
                              sizeof(err)) == BINK_ENC_OK);
    CHECK(memcmp(loaded, rgb, sizeof(rgb)) == 0);
    free(loaded);
    loaded = NULL;
    remove(path);

    path_for(path, sizeof(path), "tiny32.bmp");
    CHECK(write_bmp(path, rgb, 2, 2, 32, 0x7f, 1) == 0);
    CHECK(bink_enc_image_load(path, &loaded, &width, &height, err,
                              sizeof(err)) == BINK_ENC_OK);
    CHECK(memcmp(loaded, rgb, sizeof(rgb)) == 0);
    free(loaded);
    remove(path);

    path_for(path, sizeof(path), "tiny16.wav");
    CHECK(write_wav(path, 22050, 1, 16, wav16, 1) == 0);
    memset(&audio, 0, sizeof(audio));
    CHECK(bink_enc_wav_load(path, &audio, err, sizeof(err)) == BINK_ENC_OK);
    CHECK(audio.rate == 22050 && audio.channels == 1 && audio.bits == 16);
    CHECK(audio.sample_count == 1 && audio.pcm[0] == 1000);
    CHECK(audio.reduced_from_24 == 0);
    free(audio.pcm);
    remove(path);

    path_for(path, sizeof(path), "tiny8.wav");
    CHECK(write_wav(path, 8000, 1, 8, wav8, 1) == 0);
    memset(&audio, 0, sizeof(audio));
    CHECK(bink_enc_wav_load(path, &audio, err, sizeof(err)) == BINK_ENC_OK);
    CHECK(audio.pcm[0] == (int16_t)((255 - 128) << 8));
    free(audio.pcm);
    remove(path);

    path_for(path, sizeof(path), "tiny24.wav");
    CHECK(write_wav(path, 16000, 1, 24, wav24, 2) == 0);
    memset(&audio, 0, sizeof(audio));
    CHECK(bink_enc_wav_load(path, &audio, err, sizeof(err)) == BINK_ENC_OK);
    CHECK(audio.reduced_from_24 == 1);
    CHECK(audio.sample_count == 2);
    CHECK(audio.pcm[0] == 258);
    CHECK(audio.pcm[1] == -256);
    free(audio.pcm);
    remove(path);

    {
        /* 2x2 RGB PNG: (10,20,30) (40,50,60) / (70,80,90) (100,110,120). */
        static const unsigned char png2x2[] = {
            137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
            0, 0, 0, 2, 0, 0, 0, 2, 8, 2, 0, 0, 0, 253, 212, 154,
            115, 0, 0, 0, 22, 73, 68, 65, 84, 120, 218, 99, 224, 18, 145, 211,
            48, 178, 97, 112, 11, 136, 74, 201, 171, 0, 0, 15, 24, 3, 13, 185,
            185, 232, 100, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130
        };
        /* 8x8 solid RGB (220, 30, 40). */
        static const unsigned char png8x8[] = {
            137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
            0, 0, 0, 8, 0, 0, 0, 8, 8, 2, 0, 0, 0, 75, 109, 41,
            220, 0, 0, 0, 17, 73, 68, 65, 84, 120, 218, 99, 184, 35, 167, 129,
            21, 49, 12, 45, 9, 0, 129, 108, 72, 129, 255, 61, 221, 204, 0, 0,
            0, 0, 73, 69, 78, 68, 174, 66, 96, 130
        };
        /* 1x1 RGBA (10, 20, 30, 128). Alpha is dropped, not composited. */
        static const unsigned char png_rgba[] = {
            137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
            0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0, 31, 21, 196,
            137, 0, 0, 0, 13, 73, 68, 65, 84, 120, 156, 99, 224, 18, 145, 107,
            0, 0, 1, 37, 0, 189, 238, 70, 57, 30, 0, 0, 0, 0, 73, 69,
            78, 68, 174, 66, 96, 130
        };
        /* 8x8 solid JPEG. This file decodes back to RGB (220, 30, 40). */
        static const unsigned char jpeg8x8[] = {
            255, 216, 255, 224, 0, 16, 74, 70, 73, 70, 0, 1, 1, 0, 0, 72,
            0, 72, 0, 0, 255, 225, 0, 76, 69, 120, 105, 102, 0, 0, 77, 77,
            0, 42, 0, 0, 0, 8, 0, 1, 135, 105, 0, 4, 0, 0, 0, 1,
            0, 0, 0, 26, 0, 0, 0, 0, 0, 3, 160, 1, 0, 3, 0, 0,
            0, 1, 0, 1, 0, 0, 160, 2, 0, 4, 0, 0, 0, 1, 0, 0,
            0, 8, 160, 3, 0, 4, 0, 0, 0, 1, 0, 0, 0, 8, 0, 0,
            0, 0, 255, 237, 0, 56, 80, 104, 111, 116, 111, 115, 104, 111, 112, 32,
            51, 46, 48, 0, 56, 66, 73, 77, 4, 4, 0, 0, 0, 0, 0, 0,
            56, 66, 73, 77, 4, 37, 0, 0, 0, 0, 0, 16, 212, 29, 140, 217,
            143, 0, 178, 4, 233, 128, 9, 152, 236, 248, 66, 126, 255, 192, 0, 17,
            8, 0, 8, 0, 8, 3, 1, 34, 0, 2, 17, 1, 3, 17, 1, 255,
            196, 0, 31, 0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0,
            0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
            255, 196, 0, 181, 16, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4,
            4, 0, 0, 1, 125, 1, 2, 3, 0, 4, 17, 5, 18, 33, 49, 65,
            6, 19, 81, 97, 7, 34, 113, 20, 50, 129, 145, 161, 8, 35, 66, 177,
            193, 21, 82, 209, 240, 36, 51, 98, 114, 130, 9, 10, 22, 23, 24, 25,
            26, 37, 38, 39, 40, 41, 42, 52, 53, 54, 55, 56, 57, 58, 67, 68,
            69, 70, 71, 72, 73, 74, 83, 84, 85, 86, 87, 88, 89, 90, 99, 100,
            101, 102, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122, 131, 132,
            133, 134, 135, 136, 137, 138, 146, 147, 148, 149, 150, 151, 152, 153, 154, 162,
            163, 164, 165, 166, 167, 168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185,
            186, 194, 195, 196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215,
            216, 217, 218, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 241, 242, 243,
            244, 245, 246, 247, 248, 249, 250, 255, 196, 0, 31, 1, 0, 3, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 2, 3,
            4, 5, 6, 7, 8, 9, 10, 11, 255, 196, 0, 181, 17, 0, 2, 1,
            2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119, 0, 1, 2,
            3, 17, 4, 5, 33, 49, 6, 18, 65, 81, 7, 97, 113, 19, 34, 50,
            129, 8, 20, 66, 145, 161, 177, 193, 9, 35, 51, 82, 240, 21, 98, 114,
            209, 10, 22, 36, 52, 225, 37, 241, 23, 24, 25, 26, 38, 39, 40, 41,
            42, 53, 54, 55, 56, 57, 58, 67, 68, 69, 70, 71, 72, 73, 74, 83,
            84, 85, 86, 87, 88, 89, 90, 99, 100, 101, 102, 103, 104, 105, 106, 115,
            116, 117, 118, 119, 120, 121, 122, 130, 131, 132, 133, 134, 135, 136, 137, 138,
            146, 147, 148, 149, 150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167, 168,
            169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195, 196, 197, 198,
            199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217, 218, 226, 227, 228,
            229, 230, 231, 232, 233, 234, 242, 243, 244, 245, 246, 247, 248, 249, 250, 255,
            219, 0, 67, 0, 2, 2, 2, 2, 2, 2, 3, 2, 2, 3, 5, 3,
            3, 3, 5, 6, 5, 5, 5, 5, 6, 8, 6, 6, 6, 6, 6, 8,
            10, 8, 8, 8, 8, 8, 8, 10, 10, 10, 10, 10, 10, 10, 10, 12,
            12, 12, 12, 12, 12, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 15,
            15, 15, 15, 15, 255, 219, 0, 67, 1, 2, 2, 2, 4, 4, 4, 7,
            4, 4, 7, 16, 11, 9, 11, 16, 16, 16, 16, 16, 16, 16, 16, 16,
            16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
            16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
            16, 16, 16, 16, 16, 16, 16, 16, 16, 255, 221, 0, 4, 0, 1, 255,
            218, 0, 12, 3, 1, 0, 2, 17, 3, 17, 0, 63, 0, 249, 126, 138,
            40, 175, 196, 207, 245, 224, 255, 217
        };
        static const uint8_t rgba_rgb[3] = { 10, 20, 30 };
        uint8_t solid[8 * 8 * 3];

        path_for(path, sizeof(path), "tiny.PNG");
        CHECK(write_bytes(path, png2x2, sizeof(png2x2)) == 0);
        loaded = NULL;
        CHECK(bink_enc_image_load(path, &loaded, &width, &height, err,
                                  sizeof(err)) == BINK_ENC_OK);
        CHECK(width == 2 && height == 2);
        CHECK(memcmp(loaded, rgb, sizeof(rgb)) == 0);
        free(loaded);
        loaded = NULL;
        remove(path);

        path_for(path, sizeof(path), "solid.png");
        CHECK(write_bytes(path, png8x8, sizeof(png8x8)) == 0);
        CHECK(bink_enc_image_load(path, &loaded, &width, &height, err,
                                  sizeof(err)) == BINK_ENC_OK);
        CHECK(width == 8 && height == 8);
        fill_rgb(solid, 8, 8, 220, 30, 40);
        CHECK(memcmp(loaded, solid, sizeof(solid)) == 0);
        CHECK(test_still("png.bik", loaded, width, height) == 0);
        free(loaded);
        loaded = NULL;
        remove(path);

        path_for(path, sizeof(path), "alpha.png");
        CHECK(write_bytes(path, png_rgba, sizeof(png_rgba)) == 0);
        CHECK(bink_enc_image_load(path, &loaded, &width, &height, err,
                                  sizeof(err)) == BINK_ENC_OK);
        CHECK(width == 1 && height == 1);
        CHECK(memcmp(loaded, rgba_rgb, sizeof(rgba_rgb)) == 0);
        free(loaded);
        loaded = NULL;
        remove(path);

        path_for(path, sizeof(path), "solid.JPEG");
        CHECK(write_bytes(path, jpeg8x8, sizeof(jpeg8x8)) == 0);
        CHECK(bink_enc_image_load(path, &loaded, &width, &height, err,
                                  sizeof(err)) == BINK_ENC_OK);
        CHECK(width == 8 && height == 8);
        CHECK(memcmp(loaded, solid, sizeof(solid)) == 0);
        CHECK(test_still("jpeg.bik", loaded, width, height) == 0);
        free(loaded);
        loaded = NULL;
        remove(path);

        path_for(path, sizeof(path), "bad.png");
        CHECK(write_bytes(path, (const unsigned char *)"not a png", 9) == 0);
        loaded = NULL;
        err[0] = '\0';
        CHECK(bink_enc_image_load(path, &loaded, &width, &height, err,
                                  sizeof(err)) == BINK_ENC_ERR_INPUT);
        CHECK(loaded == NULL);
        CHECK(err[0] != '\0');
        remove(path);

        path_for(path, sizeof(path), "frame.tga");
        CHECK(write_bytes(path, png2x2, sizeof(png2x2)) == 0);
        err[0] = '\0';
        CHECK(bink_enc_image_load(path, &loaded, &width, &height, err,
                                  sizeof(err)) == BINK_ENC_ERR_INPUT);
        CHECK(strcmp(err, "image must be .ppm, .bmp, .png, .jpg, or .jpeg") == 0);
        remove(path);
    }
    return 0;
}

#ifdef BINK_BUILD_EXE
static int run_build(const char *cmd)
{
    int status = system(cmd);

    if (status == -1 || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static int test_cli(void)
{
    char dir[1024];
    char frame2[1100];
    char frame10[1100];
    char out[1100];
    char cmd[4096];
    uint8_t green[8 * 8 * 3];
    uint8_t red[8 * 8 * 3];
    uint8_t y[8 * 8];
    uint8_t u[4 * 4];
    uint8_t v[4 * 4];
    bink movie;
    const unsigned char *plane;
    int stride;
    int code;

    path_for(dir, sizeof(dir), "frames");
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return 1;
    snprintf(frame2, sizeof(frame2), "%s/frame2.ppm", dir);
    snprintf(frame10, sizeof(frame10), "%s/frame10.ppm", dir);
    path_for(out, sizeof(out), "order.bik");
    fill_rgb(green, 8, 8, 0, 255, 0);
    fill_rgb(red, 8, 8, 255, 0, 0);
    CHECK(write_ppm(frame2, green, 8, 8) == 0);
    CHECK(write_ppm(frame10, red, 8, 8) == 0);
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" -o \"%s\" --fps 10 --quiet",
             BINK_BUILD_EXE, dir, out);
    code = run_build(cmd);
    if (code != 0) {
        fprintf(stderr, "FAIL bink_build order exited %d\n", code);
        return 1;
    }
    movie = bink_open_file(out, BINK_MODE_DISK);
    CHECK(movie != NULL);
    CHECK(bink_enable_video(movie, 1) == 0);
    CHECK(bink_first(movie) == BINK_MORE);
    bink_enc_rgb_to_yuv(green, 8, 8, y, u, v);
    plane = bink_get_plane(movie, BINK_PLANE_Y);
    stride = bink_get_stride(movie, BINK_PLANE_Y);
    CHECK(plane != NULL && stride > 0);
    CHECK(plane[0] == y[0]);
    CHECK(bink_next(movie) == BINK_LAST);
    bink_enc_rgb_to_yuv(red, 8, 8, y, u, v);
    plane = bink_get_plane(movie, BINK_PLANE_Y);
    CHECK(plane != NULL && plane[0] == y[0]);
    bink_close(movie);
    remove(out);
    remove(frame2);
    remove(frame10);
    rmdir(dir);

    snprintf(cmd, sizeof(cmd), "\"%s\" --help", BINK_BUILD_EXE);
    CHECK(run_build(cmd) == 0);
    snprintf(cmd, sizeof(cmd), "\"%s\" --not-an-option", BINK_BUILD_EXE);
    CHECK(run_build(cmd) == BINK_ENC_ERR_USAGE);

    {
        static const unsigned char png2x2[] = {
            137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
            0, 0, 0, 2, 0, 0, 0, 2, 8, 2, 0, 0, 0, 253, 212, 154,
            115, 0, 0, 0, 22, 73, 68, 65, 84, 120, 218, 99, 224, 18, 145, 211,
            48, 178, 97, 112, 11, 136, 74, 201, 171, 0, 0, 15, 24, 3, 13, 185,
            185, 232, 100, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130
        };
        char png_dir[1024];
        char frame2[1100];
        char frame10[1100];

        path_for(png_dir, sizeof(png_dir), "pngframes");
        if (mkdir(png_dir, 0755) != 0 && errno != EEXIST)
            return 1;
        snprintf(frame2, sizeof(frame2), "%s/frame2.png", png_dir);
        snprintf(frame10, sizeof(frame10), "%s/frame10.PNG", png_dir);
        path_for(out, sizeof(out), "pngorder.bik");
        CHECK(write_bytes(frame2, png2x2, sizeof(png2x2)) == 0);
        CHECK(write_bytes(frame10, png2x2, sizeof(png2x2)) == 0);
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" \"%s\" -o \"%s\" --fps 10 --quiet --verify",
                 BINK_BUILD_EXE, png_dir, out);
        code = run_build(cmd);
        if (code != 0) {
            fprintf(stderr, "FAIL bink_build png directory exited %d\n", code);
            return 1;
        }
        movie = bink_open_file(out, BINK_MODE_DISK);
        CHECK(movie != NULL);
        CHECK(bink_enable_video(movie, 1) == 0);
        CHECK(bink_first(movie) == BINK_MORE);
        CHECK(bink_next(movie) == BINK_LAST);
        bink_close(movie);
        remove(out);
        remove(frame2);
        remove(frame10);
        rmdir(png_dir);
    }
    return 0;
}
#endif

int main(void)
{
    bink_enc_options opt;

    bink_enc_options_default(&opt);
    CHECK(opt.keyframe_interval == 150);
    CHECK(opt.motion_range == 8);
    if (test_pictures() != 0)
        return 1;
    if (test_skip_and_motion() != 0)
        return 1;
    if (test_intra_dct() != 0)
        return 1;
    if (test_inter_dct() != 0)
        return 1;
    if (test_inter_dct_no_motion_search() != 0)
        return 1;
    if (test_inter_wrap_safety() != 0)
        return 1;
    if (test_inter_dc_range() != 0)
        return 1;
    if (test_inter_chain() != 0)
        return 1;
    if (test_dct_roundtrip() != 0)
        return 1;
    if (test_coeff_writer_roundtrip() != 0)
        return 1;
    if (test_coeff_writer_high_planes() != 0)
        return 1;
    if (test_cbits() != 0)
        return 1;
    if (test_quantize_rejects_out_of_range() != 0)
        return 1;
    if (test_audio() != 0)
        return 1;
    if (test_audio_sine_frequency() != 0)
        return 1;
    if (test_audio_sine_snr() != 0)
        return 1;
    if (test_audio_silence_rle() != 0)
        return 1;
    if (test_audio_quiet_bands() != 0)
        return 1;
    if (test_audio_size() != 0)
        return 1;
    if (test_audio_stereo_channels() != 0)
        return 1;
    if (test_audio_rates() != 0)
        return 1;
    if (test_audio_dct_forward() != 0)
        return 1;
    if (test_failure_keeps_or_removes() != 0)
        return 1;
    if (test_image_and_wav_load() != 0)
        return 1;
#ifdef BINK_BUILD_EXE
    if (test_cli() != 0)
        return 1;
#endif
    return 0;
}
