#ifndef LIBBINK_VIDEO_H
#define LIBBINK_VIDEO_H

#include "bink_bits.h"

#include <stddef.h>

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

typedef struct BinkVideo {
    unsigned long width;
    unsigned long height;
    int version;
    int has_alpha;
    int swap_planes;

    unsigned char *cur[4];
    unsigned char *ref[4];
    int stride[4];
    int plane_width[4];
    int plane_height[4];
    int alloc_height[4];

    int bw;
    int bh;
    int cbw;
    int cbh;

    BinkBundle bundles[BINK_NB_SRC];
    unsigned char *bundle_data;
    BinkTree col_high[16];
    int col_lastval;

    int has_prev;

    unsigned char *rgb;
    unsigned long rgb_size;
    int rgb_valid;
} BinkVideo;

int bink_video_init(BinkVideo *v, unsigned long width, unsigned long height,
                    unsigned long flags, int version);
void bink_video_uninit(BinkVideo *v);
void bink_video_reset(BinkVideo *v);
int bink_video_decode(BinkVideo *v, const unsigned char *data, size_t size);
unsigned char *bink_video_plane(BinkVideo *v, int plane);
int bink_video_stride(const BinkVideo *v, int plane);
int bink_video_plane_width(const BinkVideo *v, int plane);
int bink_video_plane_height(const BinkVideo *v, int plane);
const unsigned char *bink_video_rgb(BinkVideo *v);

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
