#include "bink_dsp.h"
#include "test_common.h"

static int test_idct_put(void)
{
    int32_t block[64] = {0};
    unsigned char dst[8 * 8];
    int i;

    block[0] = 0x1000;
    memset(dst, 0, sizeof(dst));
    bink_idct_put(dst, 8, block);
    for (i = 0; i < 64; i++)
        CHECK(dst[i] == 16);
    return 0;
}

static int test_idct_add(void)
{
    int32_t block[64] = {0};
    unsigned char dst[8 * 8];
    int i;

    block[0] = 0x1000;
    memset(dst, 100, sizeof(dst));
    bink_idct_add(dst, 8, block);
    for (i = 0; i < 64; i++)
        CHECK(dst[i] == 116);
    return 0;
}

static int test_scale_block(void)
{
    unsigned char src[64];
    unsigned char dst[16 * 32];
    int i, j;

    for (i = 0; i < 64; i++)
        src[i] = (unsigned char)i;
    memset(dst, 0xAA, sizeof(dst));
    bink_scale_block(src, dst, 32);
    for (j = 0; j < 8; j++) {
        for (i = 0; i < 8; i++) {
            CHECK(dst[(2 * j) * 32 + 2 * i] == src[j * 8 + i]);
            CHECK(dst[(2 * j) * 32 + 2 * i + 1] == src[j * 8 + i]);
            CHECK(dst[(2 * j + 1) * 32 + 2 * i] == src[j * 8 + i]);
            CHECK(dst[(2 * j + 1) * 32 + 2 * i + 1] == src[j * 8 + i]);
        }
    }
    CHECK(dst[16 * 32 - 1] == 0xAA);
    return 0;
}

static int test_add_pixels8(void)
{
    int16_t block[64];
    unsigned char dst[8 * 8];
    int i;

    for (i = 0; i < 64; i++)
        block[i] = -10;
    memset(dst, 200, sizeof(dst));
    bink_add_pixels8(dst, block, 8);
    for (i = 0; i < 64; i++)
        CHECK(dst[i] == 190);

    for (i = 0; i < 64; i++)
        block[i] = 10;
    memset(dst, 250, sizeof(dst));
    bink_add_pixels8(dst, block, 8);
    for (i = 0; i < 64; i++)
        CHECK(dst[i] == 4);
    return 0;
}

static int test_fill_and_copy(void)
{
    unsigned char buf[16 * 16];
    unsigned char src[8 * 8];
    unsigned char copied[8 * 8];
    int i;

    memset(buf, 0, sizeof(buf));
    bink_fill_block(buf, 16, 8, 7);
    for (i = 0; i < 8 * 16; i++)
        CHECK((i % 16 < 8) ? buf[i] == 7 : buf[i] == 0);
    CHECK(buf[8 * 16 + 0] == 0);

    for (i = 0; i < 64; i++)
        src[i] = (unsigned char)(i + 1);
    memset(copied, 0, sizeof(copied));
    bink_copy_block(copied, src, 8);
    CHECK(memcmp(copied, src, 64) == 0);
    return 0;
}

int main(void)
{
    if (test_idct_put() != 0)
        return 1;
    if (test_idct_add() != 0)
        return 1;
    if (test_scale_block() != 0)
        return 1;
    if (test_add_pixels8() != 0)
        return 1;
    if (test_fill_and_copy() != 0)
        return 1;
    return 0;
}
