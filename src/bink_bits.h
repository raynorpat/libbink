#ifndef LIBBINK_BITS_H
#define LIBBINK_BITS_H

#include <stddef.h>

/** LSB-first bit reader over a byte buffer. */
typedef struct {
    const unsigned char *data;
    size_t size;
    size_t bitpos;
} BinkBits;

/** Huffman tree descriptor used by the bundle readers. */
typedef struct {
    int vlc_num;
    unsigned char syms[16];
} BinkTree;

void bink_bits_init(BinkBits *bits, const unsigned char *data, size_t size);
/** Read count bits LSB-first, or -1 when fewer bits remain. */
int bink_bits_read(BinkBits *bits, int count);
int bink_bits_read1(BinkBits *bits);
void bink_bits_skip(BinkBits *bits, int count);
int bink_bits_left(const BinkBits *bits);
void bink_bits_align32(BinkBits *bits);

/** Build the static Huffman lookup tables (idempotent). */
void bink_huff_init(void);
/** Read a tree descriptor from the bitstream. */
int bink_tree_read(BinkBits *bits, BinkTree *tree);
/** Decode one symbol using the tree's vlc_num. */
int bink_tree_decode(BinkBits *bits, const BinkTree *tree);

#endif
