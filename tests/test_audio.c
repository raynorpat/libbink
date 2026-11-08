#include "bink.h"
#include "bink_internal.h"
#include "reference_audio.h"
#include "test_common.h"

static void w16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void w32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void fill_one_track(unsigned char *buf)
{
    memset(buf, 0, 68);
    memcpy(buf, "BIKi", 4);
    w32(buf + 4, 60);          /* file size field: buffer length minus 8 */
    w32(buf + 8, 1);           /* one frame */
    w32(buf + 12, 8);          /* largest frame */
    w32(buf + 20, 64);         /* width */
    w32(buf + 24, 64);         /* height */
    w32(buf + 28, 30);         /* fps numerator */
    w32(buf + 32, 1);          /* fps denominator */
    w32(buf + 40, 1);          /* one audio track */
    w32(buf + 44, 134400);     /* maximum decoded size */
    w16(buf + 48, 44100);      /* sample rate */
    w16(buf + 50, 0x3000);     /* DCT and stereo */
    w32(buf + 52, 0);          /* track id */
    w32(buf + 56, 60);         /* frame 0 offset */
    w32(buf + 60, 4);          /* 4-byte audio chunk, empty video tail */
}

static int test_synthetic_max_decoded(void)
{
    unsigned char buf[68];
    bink b;

    fill_one_track(buf);
    b = bink_open_memory(buf, sizeof(buf));
    CHECK(b != NULL);
    CHECK(b->audio_tracks == 1);
    CHECK(b->audio[0].max_decoded == 134400);
    bink_close(b);
    return 0;
}

static int test_enable_without_pcm(void)
{
    unsigned char buf[68];
    char status;
    bink b;

    CHECK(bink_enable_audio(NULL, 0, 1) == -1);

    fill_one_track(buf);
    b = bink_open_memory(buf, sizeof(buf));
    CHECK(b != NULL);
    CHECK(bink_enable_audio(b, 1, 1) == -1);
    CHECK(bink_enable_audio(b, 0, 1) == 0);
    CHECK(bink_get_audio(b, 0) == NULL);
    CHECK(bink_get_audio_size(b, 0) == 0);

    status = bink_first(b);
    CHECK(status == BINK_MORE || status == BINK_LAST);
    CHECK(bink_get_audio(b, 0) == NULL);
    CHECK(bink_get_audio_size(b, 0) == 0);

    CHECK(bink_enable_audio(b, 0, 0) == 0);
    status = bink_first(b);
    CHECK(status != BINK_ERROR);
    bink_close(b);
    return 0;
}

static int expect_max_decoded(const char *name, int tracks,
                              unsigned long max_decoded)
{
    const char *dir = test_media_dir();
    char path[1024];
    bink b;

    if (dir == NULL)
        return 0;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    b = bink_open_file(path, BINK_MODE_DISK);
    CHECK(b != NULL);
    CHECK(b->audio_tracks == tracks);
    if (tracks > 0)
        CHECK(b->audio[0].max_decoded == max_decoded);
    bink_close(b);
    return 0;
}

static int test_real_max_decoded(void)
{
    if (test_media_dir() == NULL)
        return 0;
    if (expect_max_decoded("logo_lucas.bik", 1, 134400) != 0)
        return 1;
    if (expect_max_decoded("valve.bik", 1, 138240) != 0)
        return 1;
    if (expect_max_decoded("original.bik", 1, 138240) != 0)
        return 1;
    if (expect_max_decoded("ATI-9700-Animusic-Movie-v1.0.bik", 1, 145920) != 0)
        return 1;
    if (expect_max_decoded("Robotica_1080_bink_4100k.bik", 0, 0) != 0)
        return 1;

    {
        char path[1024];
        bink b;

        snprintf(path, sizeof(path), "%s/Robotica_1080_bink_4100k.bik",
                 test_media_dir());
        b = bink_open_file(path, BINK_MODE_DISK);
        CHECK(b != NULL);
        CHECK(bink_enable_audio(b, 0, 1) == -1);
        bink_close(b);
    }
    return 0;
}

static const struct reference_audio *find_audio_ref(const char *file)
{
    size_t i;

    for (i = 0; i < sizeof(reference_audio) / sizeof(reference_audio[0]); i++) {
        if (strcmp(reference_audio[i].file, file) == 0)
            return &reference_audio[i];
    }
    return NULL;
}

static int walk_dct_file(const char *file, int enable_video)
{
    const char *dir = test_media_dir();
    const struct reference_audio *ref = NULL;
    char path[1024];
    unsigned char *stream = NULL;
    unsigned long got = 0;
    unsigned long frames = 0;
    unsigned long frame = 0;
    int saw_empty = 0;
    char status;
    bink b;

    if (dir == NULL)
        return 0;

    ref = find_audio_ref(file);
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    b = bink_open_file(path, BINK_MODE_DISK);
    CHECK(b != NULL);
    CHECK(bink_info_all(b, NULL, &frames, NULL) == 0);
    CHECK(bink_enable_audio(b, 0, 1) == 0);
    if (enable_video)
        CHECK(bink_enable_video(b, 1) == 0);

    status = bink_first(b);
    while (status == BINK_MORE || status == BINK_LAST) {
        unsigned long size = bink_get_audio_size(b, 0);
        const unsigned char *pcm = bink_get_audio(b, 0);

        if (size == 0) {
            CHECK(pcm == NULL);
            if (frame > 0)
                saw_empty = 1;
        } else {
            unsigned char *grown;

            CHECK(pcm != NULL);
            grown = (unsigned char *)realloc(stream, (size_t)got + (size_t)size);
            CHECK(grown != NULL);
            stream = grown;
            memcpy(stream + got, pcm, (size_t)size);
            got += size;
        }

        if (frame == 0 && ref != NULL) {
            CHECK(size == ref->frame0_bytes);
            CHECK(fnv1a(pcm, (size_t)size) == ref->frame0_hash);
        }
        if (frame == 0 &&
            strcmp(file, "ATI-9700-Animusic-Movie-v1.0.bik") == 0)
            CHECK(size == 145920);
        if (ref != NULL && frame == frames / 2) {
            CHECK(got > ref->frame0_bytes);
            CHECK(got < ref->pcm_bytes);
        }

        if (status == BINK_LAST)
            break;
        status = bink_next(b);
        frame++;
    }
    CHECK(status != BINK_ERROR);

    if (ref != NULL) {
        CHECK(saw_empty);
        CHECK(got == ref->pcm_bytes);
        CHECK(fnv1a(stream, (size_t)got) == ref->hash);
    }
    free(stream);
    bink_close(b);
    return 0;
}

static int test_dct_matches_reference(void)
{
    if (test_media_dir() == NULL)
        return 0;
    if (walk_dct_file("logo_lucas.bik", 1) != 0)
        return 1;
    if (walk_dct_file("valve.bik", 1) != 0)
        return 1;
    if (walk_dct_file("original.bik", 1) != 0)
        return 1;
    if (walk_dct_file("ATI-9700-Animusic-Movie-v1.0.bik", 0) != 0)
        return 1;
    return 0;
}

int main(void)
{
    if (test_synthetic_max_decoded() != 0)
        return 1;
    if (test_enable_without_pcm() != 0)
        return 1;
    if (test_real_max_decoded() != 0)
        return 1;
    if (test_dct_matches_reference() != 0)
        return 1;
    return 0;
}
