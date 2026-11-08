#include "bink.h"
#include "test_common.h"

static void w32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void build_three_frames(unsigned char *buf)
{
    int i;

    memset(buf, 0, 116);
    memcpy(buf, "BIKi", 4);
    w32(buf + 4, 108);       /* file size field */
    w32(buf + 8, 3);         /* frame count */
    w32(buf + 12, 16);       /* largest frame */
    w32(buf + 20, 64);       /* width */
    w32(buf + 24, 64);       /* height */
    w32(buf + 28, 30);       /* fps numerator */
    w32(buf + 32, 1);        /* fps denominator */
    w32(buf + 36, 0);        /* video flags */
    w32(buf + 40, 1);        /* audio tracks */
    /* 44: skipped max decoded size */
    buf[48] = 0x44;          /* rate 44100 = 0xAC44 */
    buf[49] = 0xAC;
    /* 50: track flags */
    /* 52: skipped track id */
    w32(buf + 56, 68);       /* frame 0 offset */
    w32(buf + 60, 84);       /* frame 1 offset */
    w32(buf + 64, 100);      /* frame 2 offset */

    for (i = 0; i < 3; i++) {
        unsigned long off = 68 + (unsigned long)i * 16;

        w32(buf + off, 4);       /* audio chunk size */
        w32(buf + off + 8, 4);   /* video packet size */
    }
}

static int test_synthetic(void)
{
    unsigned char buf[116];
    unsigned long cur = 99;
    bink b;

    build_three_frames(buf);
    b = bink_open_memory(buf, 116);
    CHECK(b != NULL);

    CHECK(bink_first(b) == BINK_MORE);
    CHECK(bink_info_all(b, &cur, NULL, NULL) == 0 && cur == 0);
    CHECK(bink_next(b) == BINK_MORE);
    CHECK(bink_info_all(b, &cur, NULL, NULL) == 0 && cur == 1);
    CHECK(bink_next(b) == BINK_LAST);
    CHECK(bink_info_all(b, &cur, NULL, NULL) == 0 && cur == 2);
    CHECK(bink_next(b) == BINK_DONE);

    CHECK(bink_seek_keyframe(b, 2) == 0);
    CHECK(bink_info_all(b, &cur, NULL, NULL) == 0 && cur == 0);

    CHECK(bink_seek_keyframe(b, 999) == 0);
    CHECK(bink_info_all(b, &cur, NULL, NULL) == 0 && cur == 0);

    bink_close(b);
    return 0;
}

static int test_real_media(void)
{
    const char *dir = test_media_dir();
    char path[1024];
    unsigned long cur = 0;
    unsigned long count = 0;
    char status;
    bink b;

    if (dir == NULL)
        return 0;

    snprintf(path, sizeof(path), "%s/logo_lucas.bik", dir);
    b = bink_open_file(path, BINK_MODE_DISK);
    CHECK(b != NULL);

    status = bink_first(b);
    CHECK(status == BINK_MORE || status == BINK_LAST);
    count = 1;
    while ((status = bink_next(b)) == BINK_MORE ||
           status == BINK_LAST) {
        count++;
        if (status == BINK_LAST)
            break;
    }
    CHECK(status != BINK_ERROR);
    CHECK(count == 266);
    CHECK(bink_info_all(b, &cur, NULL, NULL) == 0 && cur == 265);

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
