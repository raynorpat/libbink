#include "bink_bits.h"
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

static void bw_init_bits(BinkBits *bits, struct bitwriter *bw)
{
    bink_bits_init(bits, bw->data, (bw->bits + 7) / 8);
}

static int test_bitreader(void)
{
    static const unsigned char data[2] = {0x0A, 0x0C};
    BinkBits bits;

    bink_bits_init(&bits, data, sizeof(data));
    CHECK(bink_bits_left(&bits) == 16);
    CHECK(bink_bits_read(&bits, 3) == 2);
    CHECK(bink_bits_left(&bits) == 13);
    CHECK(bink_bits_read(&bits, 5) == 1);
    CHECK(bink_bits_left(&bits) == 8);
    CHECK(bink_bits_read(&bits, 9) == -1);
    CHECK(bink_bits_left(&bits) == 8);
    CHECK(bink_bits_read1(&bits) == 0);
    CHECK(bink_bits_read(&bits, 7) == 6);
    CHECK(bink_bits_left(&bits) == 0);
    CHECK(bink_bits_read1(&bits) == -1);

    bink_bits_init(&bits, data, sizeof(data));
    CHECK(bink_bits_read(&bits, 3) == 2);
    bink_bits_skip(&bits, 5);
    CHECK(bink_bits_left(&bits) == 8);
    bink_bits_align32(&bits);
    CHECK(bink_bits_left(&bits) == 0);

    bink_bits_init(&bits, data, sizeof(data));
    bink_bits_align32(&bits);
    CHECK(bink_bits_left(&bits) == 16);
    return 0;
}

static int test_tree_read(void)
{
    struct bitwriter bw;
    BinkBits bits;
    BinkTree tree;
    int i;

    /* vlc_num 0 gives the identity mapping. */
    bw_reset(&bw);
    bw_put(&bw, 0, 4);
    bw_init_bits(&bits, &bw);
    memset(&tree, 0, sizeof(tree));
    CHECK(bink_tree_read(&bits, &tree) == 0);
    CHECK(tree.vlc_num == 0);
    for (i = 0; i < 16; i++)
        CHECK(tree.syms[i] == i);

    /* Explicit symbols: one given symbol, then the rest in order. */
    bw_reset(&bw);
    bw_put(&bw, 1, 4);   /* vlc_num */
    bw_put(&bw, 1, 1);   /* explicit flag */
    bw_put(&bw, 0, 3);   /* length */
    bw_put(&bw, 5, 4);   /* symbol */
    bw_init_bits(&bits, &bw);
    memset(&tree, 0, sizeof(tree));
    CHECK(bink_tree_read(&bits, &tree) == 0);
    CHECK(tree.vlc_num == 1);
    for (i = 0; i < 16; i++)
        CHECK(tree.syms[i] == (i == 0 ? 5 : (i <= 5 ? i - 1 : i)));

    /* Shuffle depth 0 with zero bits is still the identity. */
    bw_reset(&bw);
    bw_put(&bw, 5, 4);   /* vlc_num */
    bw_put(&bw, 0, 1);   /* shuffle form */
    bw_put(&bw, 0, 2);   /* depth */
    for (i = 0; i < 8; i++)
        bw_put(&bw, 0, 1);
    bw_init_bits(&bits, &bw);
    memset(&tree, 0, sizeof(tree));
    CHECK(bink_tree_read(&bits, &tree) == 0);
    for (i = 0; i < 16; i++)
        CHECK(tree.syms[i] == i);

    /* Shuffle depth 1 with the first pair swapped. */
    bw_reset(&bw);
    bw_put(&bw, 5, 4);
    bw_put(&bw, 0, 1);
    bw_put(&bw, 1, 2);   /* depth */
    bw_put(&bw, 1, 1);   /* swap first pair */
    for (i = 0; i < 7; i++)
        bw_put(&bw, 0, 1);
    for (i = 0; i < 8; i++)
        bw_put(&bw, 0, 1);
    bw_init_bits(&bits, &bw);
    memset(&tree, 0, sizeof(tree));
    CHECK(bink_tree_read(&bits, &tree) == 0);
    CHECK(tree.syms[0] == 1 && tree.syms[1] == 0);
    CHECK(tree.syms[2] == 2 && tree.syms[3] == 3);

    return 0;
}

static int test_tree_decode(void)
{
    struct bitwriter bw;
    BinkBits bits;
    BinkTree tree;
    static const struct {
        int vlc;
        unsigned code;
        int len;
        int symbol;
    } cases[] = {
        {0, 0x05, 4, 5},
        {1, 0x03, 5, 2},
        {15, 0x17, 6, 7},
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bw_reset(&bw);
        bw_put(&bw, cases[i].code, cases[i].len);
        bw_init_bits(&bits, &bw);
        memset(&tree, 0, sizeof(tree));
        tree.vlc_num = cases[i].vlc;
        CHECK(bink_tree_decode(&bits, &tree) == cases[i].symbol);
        CHECK(bink_bits_left(&bits) == (int)(bits.size * 8) - cases[i].len);
    }

    bw_reset(&bw);
    bw_init_bits(&bits, &bw);
    memset(&tree, 0, sizeof(tree));
    CHECK(bink_tree_decode(&bits, &tree) == -1);
    return 0;
}

int main(void)
{
    if (test_bitreader() != 0)
        return 1;
    if (test_tree_read() != 0)
        return 1;
    if (test_tree_decode() != 0)
        return 1;
    return 0;
}
