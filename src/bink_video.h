#ifndef LIBBINK_VIDEO_H
#define LIBBINK_VIDEO_H

#include "bink_bits.h"

enum {
    BINK_SRC_BLOCK_TYPES = 0,
    BINK_SRC_SUB_BLOCK_TYPES,
    BINK_SRC_COLORS,
    BINK_SRC_PATTERN,
    BINK_SRC_X_OFF,
    BINK_SRC_Y_OFF,
    BINK_SRC_INTRA_DC,
    BINK_SRC_INTER_DC,
    BINK_SRC_RUN,
    BINK_NB_SRC
};

typedef struct {
    int len;
    BinkTree tree;
    unsigned char *data;
    unsigned char *data_end;
    unsigned char *cur_dec;
    unsigned char *cur_ptr;
} BinkBundle;

int bink_bundle_read_runs(BinkBits *bits, BinkBundle *b);
int bink_bundle_read_motion_values(BinkBits *bits, BinkBundle *b);
int bink_bundle_read_block_types(BinkBits *bits, BinkBundle *b, int version);
int bink_bundle_read_patterns(BinkBits *bits, BinkBundle *b);
int bink_bundle_read_colors(BinkBits *bits, BinkBundle *b,
                            BinkTree col_high[16], int *col_lastval,
                            int version);
int bink_bundle_read_dcs(BinkBits *bits, BinkBundle *b,
                         int start_bits, int has_sign);

#endif
