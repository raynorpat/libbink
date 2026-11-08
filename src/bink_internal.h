#ifndef LIBBINK_INTERNAL_H
#define LIBBINK_INTERNAL_H

#include "bink.h"
#include "bink_video.h"

#define BINK_MAX_AUDIO_TRACKS 7
#define BINK_MAX_FRAMES 1000000UL
#define BINK_MAX_WIDTH 7680UL
#define BINK_MAX_HEIGHT 4800UL

typedef struct {
    unsigned long rate;
    unsigned char channels;
    unsigned char bitdepth;
    unsigned short flags;
    unsigned long max_decoded;
} BinkAudioTrack;

struct bink_t {
    /* input source */
    FILE *file;
    unsigned char *mem;
    unsigned long mem_size;
    unsigned char mode;
    unsigned char owns_file;

    /* container metadata */
    unsigned long file_size;
    int version; /* 'f', 'g', 'h' or 'i' */
    unsigned long width;
    unsigned long height;
    unsigned long frame_count;
    unsigned long largest_frame;
    unsigned long fps_num;
    unsigned long fps_den;
    unsigned long flags;
    int audio_tracks;
    BinkAudioTrack audio[BINK_MAX_AUDIO_TRACKS];

    /* frame index */
    unsigned long *frame_offsets;
    unsigned long *frame_sizes;
    unsigned char *frame_keyframes;

    /* current frame packet */
    unsigned char *frame_data;
    unsigned long frame_data_size;
    const unsigned char *video_data;
    unsigned long video_size;

    /* playback state */
    unsigned long current_frame;
    unsigned char video_enabled;
    BinkVideo *video;
};

#endif
