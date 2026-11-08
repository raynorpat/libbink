#include "bink_bits.h"
#include "bink_video.h"
#include "test_common.h"

struct bitwriter {
    unsigned char data[256];
    size_t bits;
};

static void bw_reset(struct bitwriter *bw)
{
    memset(bw, 0, sizeof(*bw));
}

static void bw_put(struct bitwriter *bw, unsigned value, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (value & (1u << i))
            bw->data[bw->bits >> 3] |= (unsigned char)(1u << (bw->bits & 7));
        bw->bits++;
    }
}

static void bundle_init(BinkBundle *b, unsigned char *data, size_t size,
                        int len)
{
    int i;

    memset(b, 0, sizeof(*b));
    b->len = len;
    b->tree.vlc_num = 0;
    for (i = 0; i < 16; i++)
        b->tree.syms[i] = (unsigned char)i;
    b->data = data;
    b->data_end = data + size;
    b->cur_dec = data;
    b->cur_ptr = data;
}

static int test_runs(void)
{
    struct bitwriter bw;
    BinkBits bits;
    BinkBundle b;
    unsigned char data[8];

    bw_reset(&bw);
    bw_put(&bw, 3, 2);   /* t */
    bw_put(&bw, 0, 1);   /* individual values */
    bw_put(&bw, 1, 4);
    bw_put(&bw, 2, 4);
    bw_put(&bw, 3, 4);
    bink_bits_init(&bits, bw.data, (bw.bits + 7) / 8);
    memset(data, 0, sizeof(data));
    bundle_init(&b, data, sizeof(data), 2);
    CHECK(bink_bundle_read_runs(&bits, &b) == 0);
    CHECK(data[0] == 1 && data[1] == 2 && data[2] == 3);
    CHECK(b.cur_dec == data + 3);

    bw_reset(&bw);
    bw_put(&bw, 3, 2);
    bw_put(&bw, 1, 1);   /* all same */
    bw_put(&bw, 7, 4);
    bink_bits_init(&bits, bw.data, (bw.bits + 7) / 8);
    memset(data, 0, sizeof(data));
    bundle_init(&b, data, sizeof(data), 2);
    CHECK(bink_bundle_read_runs(&bits, &b) == 0);
    CHECK(data[0] == 7 && data[1] == 7 && data[2] == 7);
    return 0;
}

static int test_motion(void)
{
    struct bitwriter bw;
    BinkBits bits;
    BinkBundle b;
    unsigned char data[8];

    bw_reset(&bw);
    bw_put(&bw, 3, 2);
    bw_put(&bw, 1, 1);   /* all same */
    bw_put(&bw, 4, 4);
    bw_put(&bw, 1, 1);   /* negative */
    bink_bits_init(&bits, bw.data, (bw.bits + 7) / 8);
    memset(data, 0, sizeof(data));
    bundle_init(&b, data, sizeof(data), 2);
    CHECK(bink_bundle_read_motion_values(&bits, &b) == 0);
    CHECK(data[0] == 0xFC && data[1] == 0xFC && data[2] == 0xFC);

    bw_reset(&bw);
    bw_put(&bw, 2, 2);
    bw_put(&bw, 0, 1);
    bw_put(&bw, 2, 4);
    bw_put(&bw, 0, 1);   /* positive */
    bw_put(&bw, 3, 4);
    bw_put(&bw, 1, 1);   /* negative */
    bink_bits_init(&bits, bw.data, (bw.bits + 7) / 8);
    memset(data, 0, sizeof(data));
    bundle_init(&b, data, sizeof(data), 2);
    CHECK(bink_bundle_read_motion_values(&bits, &b) == 0);
    CHECK(data[0] == 2 && data[1] == 0xFD);
    return 0;
}

static int test_block_types(void)
{
    struct bitwriter bw;
    BinkBits bits;
    BinkBundle b;
    unsigned char data[8];

    bw_reset(&bw);
    bw_put(&bw, 5, 3);   /* t */
    bw_put(&bw, 0, 1);
    bw_put(&bw, 1, 4);
    bw_put(&bw, 12, 4);  /* run of 4 copies of the last value */
    bink_bits_init(&bits, bw.data, (bw.bits + 7) / 8);
    memset(data, 0, sizeof(data));
    bundle_init(&b, data, sizeof(data), 3);
    CHECK(bink_bundle_read_block_types(&bits, &b, 'i') == 0);
    CHECK(data[0] == 1 && data[1] == 1 && data[2] == 1 &&
          data[3] == 1 && data[4] == 1);
    return 0;
}

static int test_patterns(void)
{
    struct bitwriter bw;
    BinkBits bits;
    BinkBundle b;
    unsigned char data[8];

    bw_reset(&bw);
    bw_put(&bw, 2, 2);
    bw_put(&bw, 1, 4);
    bw_put(&bw, 2, 4);
    bw_put(&bw, 3, 4);
    bw_put(&bw, 4, 4);
    bink_bits_init(&bits, bw.data, (bw.bits + 7) / 8);
    memset(data, 0, sizeof(data));
    bundle_init(&b, data, sizeof(data), 2);
    CHECK(bink_bundle_read_patterns(&bits, &b) == 0);
    CHECK(data[0] == 0x21 && data[1] == 0x43);
    return 0;
}

static int test_colors(void)
{
    struct bitwriter bw;
    BinkBits bits;
    BinkBundle b;
    BinkTree col_high[16];
    unsigned char data[8];
    int col_lastval = 0;
    int i;

    for (i = 0; i < 16; i++) {
        int j;

        col_high[i].vlc_num = 0;
        for (j = 0; j < 16; j++)
            col_high[i].syms[j] = (unsigned char)j;
    }

    bw_reset(&bw);
    bw_put(&bw, 2, 2);
    bw_put(&bw, 0, 1);
    bw_put(&bw, 1, 4);
    bw_put(&bw, 2, 4);
    bw_put(&bw, 3, 4);
    bw_put(&bw, 4, 4);
    bink_bits_init(&bits, bw.data, (bw.bits + 7) / 8);
    memset(data, 0, sizeof(data));
    bundle_init(&b, data, sizeof(data), 2);
    CHECK(bink_bundle_read_colors(&bits, &b, col_high, &col_lastval,
                                  'i') == 0);
    CHECK(data[0] == 0x12 && data[1] == 0x34);
    CHECK(col_lastval == 3);
    return 0;
}

static int test_dcs(void)
{
    struct bitwriter bw;
    BinkBits bits;
    BinkBundle b;
    int16_t data[8];

    bw_reset(&bw);
    bw_put(&bw, 3, 2);   /* count */
    bw_put(&bw, 100, 8);
    bw_put(&bw, 4, 4);   /* block size */
    bw_put(&bw, 5, 4);
    bw_put(&bw, 0, 1);   /* positive */
    bw_put(&bw, 3, 4);
    bw_put(&bw, 1, 1);   /* negative */
    bink_bits_init(&bits, bw.data, (bw.bits + 7) / 8);
    memset(data, 0, sizeof(data));
    bundle_init(&b, (unsigned char *)data, sizeof(data), 2);
    CHECK(bink_bundle_read_dcs(&bits, &b, 8, 0) == 0);
    CHECK(data[0] == 100 && data[1] == 105 && data[2] == 102);
    return 0;
}

int main(void)
{
    if (test_runs() != 0)
        return 1;
    if (test_motion() != 0)
        return 1;
    if (test_block_types() != 0)
        return 1;
    if (test_patterns() != 0)
        return 1;
    if (test_colors() != 0)
        return 1;
    if (test_dcs() != 0)
        return 1;
    return 0;
}
