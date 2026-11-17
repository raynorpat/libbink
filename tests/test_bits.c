#include "bink_bits.h"
#include "bink_huffdata.h"
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
        int s;

        bw_reset(&bw);
        bw_put(&bw, cases[i].code, cases[i].len);
        bw_init_bits(&bits, &bw);
        memset(&tree, 0, sizeof(tree));
        tree.vlc_num = cases[i].vlc;
        for (s = 0; s < 16; s++)
            tree.syms[s] = (unsigned char)s;
        CHECK(bink_tree_decode(&bits, &tree) == cases[i].symbol);
        CHECK(bink_bits_left(&bits) == (int)(bits.size * 8) - cases[i].len);
    }

    bw_reset(&bw);
    bw_init_bits(&bits, &bw);
    memset(&tree, 0, sizeof(tree));
    CHECK(bink_tree_decode(&bits, &tree) == -1);
    return 0;
}

static int test_bitwriter(void)
{
    BinkBitWriter bw;
    BinkBits bits;
    BinkTree tree;
    int i;

    bink_bw_init(&bw);
    CHECK(bink_bw_put(&bw, 0x0A, 4) == 0);
    CHECK(bink_bw_put(&bw, 0x15, 5) == 0);
    CHECK(bink_bw_put(&bw, 1, 1) == 0);
    CHECK(bink_bw_put(&bw, 0xABCD, 16) == 0);
    CHECK(bink_bw_data(&bw) != NULL);
    bink_bits_init(&bits, bink_bw_data(&bw), bink_bw_bytes(&bw));
    CHECK(bink_bits_read(&bits, 4) == 0x0A);
    CHECK(bink_bits_read(&bits, 5) == 0x15);
    CHECK(bink_bits_read1(&bits) == 1);
    CHECK(bink_bits_read(&bits, 16) == 0xABCD);
    bink_bw_free(&bw);

    /* The audio size word is 32 bits. The reader skips that field. */
    bink_bw_init(&bw);
    CHECK(bink_bw_put(&bw, 0x01020304u, 32) == 0);
    CHECK(bink_bw_bytes(&bw) == 4);
    CHECK(bink_bw_data(&bw)[0] == 0x04);
    CHECK(bink_bw_data(&bw)[1] == 0x03);
    CHECK(bink_bw_data(&bw)[2] == 0x02);
    CHECK(bink_bw_data(&bw)[3] == 0x01);
    bink_bits_init(&bits, bink_bw_data(&bw), bink_bw_bytes(&bw));
    bink_bits_skip(&bits, 32);
    CHECK(bink_bits_left(&bits) == 0);
    bink_bw_free(&bw);

    bink_bw_init(&bw);
    CHECK(bink_bw_put(&bw, 1, 1) == 0);
    CHECK(bink_bw_align32(&bw) == 0);
    CHECK(bink_bw_bitpos(&bw) == 32);
    CHECK(bink_bw_bytes(&bw) == 4);
    bink_bw_free(&bw);

    bink_bw_init(&bw);
    CHECK(bink_tree_write_identity(&bw, 0) == 0);
    CHECK(bink_huff_put(&bw, 0, 0) == 0);
    CHECK(bink_tree_write_identity(&bw, 5) == 0);
    CHECK(bink_huff_put(&bw, 5, 3) == 0);
    bink_bits_init(&bits, bink_bw_data(&bw), bink_bw_bytes(&bw));
    memset(&tree, 0, sizeof(tree));
    CHECK(bink_tree_read(&bits, &tree) == 0);
    CHECK(tree.vlc_num == 0);
    for (i = 0; i < 16; i++)
        CHECK(tree.syms[i] == i);
    CHECK(bink_tree_decode(&bits, &tree) == 0);
    memset(&tree, 0, sizeof(tree));
    CHECK(bink_tree_read(&bits, &tree) == 0);
    CHECK(tree.vlc_num == 5);
    for (i = 0; i < 16; i++)
        CHECK(tree.syms[i] == i);
    CHECK(bink_tree_decode(&bits, &tree) == 3);
    bink_bw_free(&bw);
    return 0;
}

/* The constant lookup tables must be exactly what the code tables imply. */
static int test_lookup_tables(void)
{
    int v, s, index;

    for (v = 0; v < 16; v++) {
        unsigned char syms[128];
        unsigned char lens[128];

        memset(syms, 0, sizeof(syms));
        memset(lens, 0, sizeof(lens));
        for (s = 0; s < 16; s++) {
            int len = bink_tree_lens[v][s];
            unsigned code = bink_tree_bits[v][s];
            unsigned mask = (1u << len) - 1u;

            for (index = 0; index < 128; index++) {
                if (((unsigned)index & mask) == code) {
                    syms[index] = (unsigned char)s;
                    lens[index] = (unsigned char)len;
                }
            }
        }
        CHECK(memcmp(syms, bink_huff_lookup_syms[v], 128) == 0);
        CHECK(memcmp(lens, bink_huff_lookup_lens[v], 128) == 0);
    }
    return 0;
}

/* Every width at every bit offset against a bit-at-a-time reference,
 * including reads that end exactly at, and run past, the end. */
static int test_reader_matches_bitwise(void)
{
    unsigned char data[23];
    unsigned seed = 12345u;
    size_t total = sizeof(data) * 8;
    size_t start;
    int count;
    size_t i;

    for (i = 0; i < sizeof(data); i++) {
        seed = seed * 1103515245u + 12345u;
        data[i] = (unsigned char)(seed >> 16);
    }
    for (start = 0; start <= total; start++) {
        for (count = 0; count <= 30; count++) {
            BinkBits bits;
            int got;

            bink_bits_init(&bits, data, sizeof(data));
            bink_bits_skip(&bits, (int)start);
            got = bink_bits_read(&bits, count);
            if (start + (size_t)count > total) {
                CHECK(got == -1);
                CHECK(bits.bitpos == start);
            } else {
                unsigned long want = 0;
                int b;

                for (b = 0; b < count; b++) {
                    size_t pos = start + (size_t)b;

                    if (data[pos >> 3] & (1u << (pos & 7)))
                        want |= 1UL << b;
                }
                CHECK(got == (int)want);
                CHECK(bits.bitpos == start + (size_t)count);
            }
        }
    }
    return 0;
}

/* The writer at every offset and width, read back with the reader. */
static int test_writer_matches_reader(void)
{
    unsigned seed = 777u;
    int offset;
    int count;

    for (offset = 0; offset < 40; offset++) {
        for (count = 1; count <= 30; count++) {
            BinkBitWriter bw;
            BinkBits bits;
            unsigned value;
            unsigned mask = (count == 32) ? ~0u : ((1u << count) - 1u);

            seed = seed * 1103515245u + 12345u;
            value = seed >> 1;
            bink_bw_init(&bw);
            CHECK(bink_bw_put(&bw, 0x15555555u, 30) == 0);
            CHECK(bink_bw_put(&bw, 0, offset % 31) == 0);
            /* High bits above count must not leak into the stream. */
            CHECK(bink_bw_put(&bw, value, count) == 0);
            CHECK(bink_bw_put(&bw, 0x2aaaaaaau, 30) == 0);
            bink_bits_init(&bits, bink_bw_data(&bw), bink_bw_bytes(&bw));
            CHECK(bink_bits_read(&bits, 30) == 0x15555555);
            CHECK(bink_bits_read(&bits, offset % 31) == 0);
            CHECK((unsigned)bink_bits_read(&bits, count) == (value & mask));
            CHECK(bink_bits_read(&bits, 30) == 0x2aaaaaaa);
            bink_bw_free(&bw);
        }
    }
    return 0;
}

int main(void)
{
    if (test_lookup_tables() != 0)
        return 1;
    if (test_reader_matches_bitwise() != 0)
        return 1;
    if (test_writer_matches_reader() != 0)
        return 1;
    if (test_bitreader() != 0)
        return 1;
    if (test_tree_read() != 0)
        return 1;
    if (test_tree_decode() != 0)
        return 1;
    if (test_bitwriter() != 0)
        return 1;
    return 0;
}
