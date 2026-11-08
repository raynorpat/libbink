#include "bink_bits.h"
#include "bink_huffdata.h"

#include <string.h>

#define BINK_TREE_MAX_BITS 7

static unsigned char huff_syms[16][1 << BINK_TREE_MAX_BITS];
static unsigned char huff_lens[16][1 << BINK_TREE_MAX_BITS];
static int huff_ready;

void bink_bits_init(BinkBits *bits, const unsigned char *data, size_t size)
{
    bits->data = data;
    bits->size = size;
    bits->bitpos = 0;
}

int bink_bits_left(const BinkBits *bits)
{
    size_t total = bits->size * 8;

    if (bits->bitpos >= total)
        return 0;
    return (int)(total - bits->bitpos);
}

int bink_bits_read(BinkBits *bits, int count)
{
    unsigned long value = 0;
    int i;

    if (count < 0 || count > 30)
        return -1;
    if ((size_t)count > bits->size * 8 - bits->bitpos)
        return -1;
    for (i = 0; i < count; i++) {
        size_t pos = bits->bitpos + (size_t)i;

        if (bits->data[pos >> 3] & (1u << (pos & 7)))
            value |= 1UL << i;
    }
    bits->bitpos += (size_t)count;
    return (int)value;
}

int bink_bits_read1(BinkBits *bits)
{
    return bink_bits_read(bits, 1);
}

void bink_bits_skip(BinkBits *bits, int count)
{
    size_t total = bits->size * 8;
    size_t next;

    if (count < 0)
        return;
    next = bits->bitpos + (size_t)count;
    bits->bitpos = (next > total) ? total : next;
}

void bink_bits_align32(BinkBits *bits)
{
    size_t total = bits->size * 8;
    size_t next = (bits->bitpos + 31) & ~(size_t)31;

    bits->bitpos = (next > total) ? total : next;
}

void bink_huff_init(void)
{
    int v;
    int s;
    int index;

    if (huff_ready)
        return;

    for (v = 0; v < 16; v++) {
        memset(huff_syms[v], 0, sizeof(huff_syms[v]));
        memset(huff_lens[v], 0, sizeof(huff_lens[v]));
        for (s = 0; s < 16; s++) {
            int len = bink_tree_lens[v][s];
            unsigned int code = bink_tree_bits[v][s];
            unsigned int mask = (1u << len) - 1u;

            for (index = 0; index < (1 << BINK_TREE_MAX_BITS); index++) {
                if ((index & mask) == code) {
                    huff_syms[v][index] = (unsigned char)s;
                    huff_lens[v][index] = (unsigned char)len;
                }
            }
        }
    }
    huff_ready = 1;
}

static void merge(BinkBits *bits, unsigned char *dst, unsigned char *src,
                  int size, int *failed)
{
    unsigned char *src2 = src + size;
    int size2 = size;

    do {
        int bit = bink_bits_read1(bits);

        if (bit < 0) {
            *failed = 1;
            return;
        }
        if (!bit) {
            *dst++ = *src++;
            size--;
        } else {
            *dst++ = *src2++;
            size2--;
        }
    } while (size && size2);
    while (size--)
        *dst++ = *src++;
    while (size2--)
        *dst++ = *src2++;
}

int bink_tree_read(BinkBits *bits, BinkTree *tree)
{
    unsigned char tmp1[16] = {0};
    unsigned char tmp2[16];
    unsigned char *in = tmp1;
    unsigned char *out = tmp2;
    int i;
    int t;
    int len;
    int flag;

    if (bink_bits_left(bits) < 4)
        return -1;

    tree->vlc_num = bink_bits_read(bits, 4);
    if (tree->vlc_num < 0)
        return -1;
    if (tree->vlc_num == 0) {
        for (i = 0; i < 16; i++)
            tree->syms[i] = (unsigned char)i;
        return 0;
    }

    flag = bink_bits_read1(bits);
    if (flag < 0)
        return -1;

    if (flag) {
        len = bink_bits_read(bits, 3);
        if (len < 0)
            return -1;
        for (i = 0; i <= len; i++) {
            int sym = bink_bits_read(bits, 4);

            if (sym < 0)
                return -1;
            tree->syms[i] = (unsigned char)sym;
            tmp1[sym] = 1;
        }
        for (i = 0; i < 16 && len < 15; i++) {
            if (!tmp1[i])
                tree->syms[++len] = (unsigned char)i;
        }
    } else {
        int failed = 0;

        len = bink_bits_read(bits, 2);
        if (len < 0)
            return -1;
        for (i = 0; i < 16; i++)
            in[i] = (unsigned char)i;
        for (i = 0; i <= len; i++) {
            int size = 1 << i;

            for (t = 0; t < 16; t += size << 1)
                merge(bits, out + t, in + t, size, &failed);
            if (failed)
                return -1;
            {
                unsigned char *swap = in;
                in = out;
                out = swap;
            }
        }
        memcpy(tree->syms, in, 16);
    }
    return 0;
}

int bink_tree_decode(BinkBits *bits, const BinkTree *tree)
{
    unsigned int peek = 0;
    size_t total;
    int i;
    int len;
    int left;

    if (tree->vlc_num < 0 || tree->vlc_num > 15)
        return -1;

    bink_huff_init();
    total = bits->size * 8;
    for (i = 0; i < BINK_TREE_MAX_BITS; i++) {
        size_t pos = bits->bitpos + (size_t)i;

        if (pos < total && (bits->data[pos >> 3] & (1u << (pos & 7))))
            peek |= 1u << i;
    }

    len = huff_lens[tree->vlc_num][peek];
    if (len == 0)
        return -1;
    left = bink_bits_left(bits);
    if (len > left)
        return -1;
    bits->bitpos += (size_t)len;
    return huff_syms[tree->vlc_num][peek];
}
