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

/**
 * Growing LSB-first bit writer. The same bit order as BinkBits, so a buffer
 * produced here can be handed to bink_bits_init.
 */
typedef struct {
    unsigned char *data;
    size_t capacity;
    size_t bitpos;
} BinkBitWriter;

void bink_bw_init(BinkBitWriter *bw);
void bink_bw_free(BinkBitWriter *bw);
void bink_bw_reset(BinkBitWriter *bw);
/** Write the low count bits of value, LSB first. count is 0..32. */
int bink_bw_put(BinkBitWriter *bw, unsigned value, int count);
/** Pad with zero bits up to the next 32-bit boundary. */
int bink_bw_align32(BinkBitWriter *bw);
size_t bink_bw_bitpos(const BinkBitWriter *bw);
size_t bink_bw_bytes(const BinkBitWriter *bw);
const unsigned char *bink_bw_data(const BinkBitWriter *bw);

/** No-op: the Huffman lookup tables are constant data. Kept for callers. */
void bink_huff_init(void);
/** Read a tree descriptor from the bitstream. */
int bink_tree_read(BinkBits *bits, BinkTree *tree);
/** Decode one symbol using the tree's vlc_num. */
int bink_tree_decode(BinkBits *bits, const BinkTree *tree);

/**
 * Pick the built-in table that encodes freq[] in the fewest bits, including
 * the identity-tree header. Symbols are 0..15 and the tree mapping is identity.
 */
int bink_huff_best(const unsigned freq[16]);
/** Write one identity-mapped symbol with the selected table. */
int bink_huff_put(BinkBitWriter *bw, int vlc, int symbol);
/**
 * Write a tree header whose symbol map is the identity. vlc 0 is four zero
 * bits. Any other table is an explicit list that starts with symbol 0, which
 * the reader fills out to 0..15.
 */
int bink_tree_write_identity(BinkBitWriter *bw, int vlc);

#endif
