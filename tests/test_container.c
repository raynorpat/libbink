#include "bink.h"
#include "test_common.h"

static void w32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void build_one_frame(unsigned char *buf, const char *sig)
{
    memset(buf, 0, 52);
    memcpy(buf, sig, 4);
    w32(buf + 4, 44);      /* file size field (actual size - 8) */
    w32(buf + 8, 1);       /* frame count */
    w32(buf + 12, 4);      /* largest frame */
    w32(buf + 20, 64);     /* width */
    w32(buf + 24, 64);     /* height */
    w32(buf + 28, 30);     /* fps numerator */
    w32(buf + 32, 1);      /* fps denominator */
    w32(buf + 36, 0);      /* video flags */
    w32(buf + 40, 0);      /* audio tracks */
    w32(buf + 44, 48);     /* frame 0 offset */
}

static int test_synthetic(void)
{
    unsigned char buf[64];
    unsigned char mask = 0xFF;
    unsigned char channels[7];
    unsigned char depths[7];
    unsigned long rates[7];
    unsigned long frames = 0;
    unsigned long w = 0, h = 0, flags = 0;
    double usf = 0.0;
    bink b;

    build_one_frame(buf, "BIKi");
    b = bink_open_memory(buf, 52);
    CHECK(b != NULL);
    CHECK(bink_info_all(b, NULL, &frames, &usf) == 0);
    CHECK(frames == 1);
    CHECK(usf > 33333.0 && usf < 33334.0);
    CHECK(bink_info_video(b, &w, &h, &flags) == 0);
    CHECK(w == 64 && h == 64 && flags == 0);
    CHECK(bink_info_audio(b, &mask, channels, depths, rates) == 0);
    CHECK(mask == 0);
    bink_close(b);

    b = bink_open_memory(buf, 4);
    CHECK(b == NULL);

    build_one_frame(buf, "BIKb");
    b = bink_open_memory(buf, 52);
    CHECK(b == NULL);

    build_one_frame(buf, "KB2a");
    b = bink_open_memory(buf, 52);
    CHECK(b == NULL);

    return 0;
}

static int test_real_media(void)
{
    const char *dir = test_media_dir();
    char path[1024];
    unsigned char mask = 0;
    unsigned char channels[7];
    unsigned char depths[7];
    unsigned long rates[7];
    unsigned long frames = 0;
    unsigned long w = 0, h = 0, flags = 0;
    bink b;

    if (dir == NULL)
        return 0;

    snprintf(path, sizeof(path), "%s/valve.bik", dir);
    b = bink_open_file(path, BINK_MODE_DISK);
    CHECK(b != NULL);
    CHECK(bink_info_all(b, NULL, &frames, NULL) == 0);
    CHECK(frames == 291);
    CHECK(bink_info_video(b, &w, &h, &flags) == 0);
    CHECK(w == 1920 && h == 1080);
    CHECK(bink_info_audio(b, &mask, channels, depths, rates) == 0);
    CHECK(mask == 0x01);
    CHECK(channels[0] == 2);
    CHECK(depths[0] == 16);
    CHECK(rates[0] == 44100);
    bink_close(b);
    return 0;
}

int main(void)
{
    if (test_synthetic() != 0)
        return 1;
    if (test_real_media() != 0)
        return 1;
    return 0;
}
