#include "bink.h"
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
