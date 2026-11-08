#include "bink.h"
#include "bink_internal.h"
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

static int test_synthetic_max_decoded(void)
{
    unsigned char buf[68];
    bink b;

    memset(buf, 0, sizeof(buf));
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

    b = bink_open_memory(buf, sizeof(buf));
    CHECK(b != NULL);
    CHECK(b->audio_tracks == 1);
    CHECK(b->audio[0].max_decoded == 134400);
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
    return 0;
}

int main(void)
{
    if (test_synthetic_max_decoded() != 0)
        return 1;
    if (test_real_max_decoded() != 0)
        return 1;
    return 0;
}
