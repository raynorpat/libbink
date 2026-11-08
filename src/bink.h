/**
    libbink - A C library for decoding Bink 1 video files
    Copyright (C) 2026 libbink contributors

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, see
    <http://www.gnu.org/licenses/>.

    The Bink 1 decoder is derived from FFmpeg's libavcodec/bink.c,
    binkdsp.c and binkdata.h, which are LGPL-2.1-or-later.
*/

#ifndef LIBBINK_BINK_H
#define LIBBINK_BINK_H

/* needed for FILE* here */
#include <stdio.h>

/** forward declaration for the opaque decoder object */
typedef struct bink_t *bink;

/** return codes from bink_first(), bink_next() and bink_seek_keyframe() */
#define BINK_DONE   0x00
#define BINK_MORE   0x01
#define BINK_LAST   0x02
#define BINK_ERROR  -1

/** file-processing mode, pass to bink_open_file() */
#define BINK_MODE_DISK    0x00
#define BINK_MODE_MEMORY  0x01

/** plane indices for bink_get_plane() */
#define BINK_PLANE_Y  0
#define BINK_PLANE_U  1
#define BINK_PLANE_V  2
#define BINK_PLANE_A  3

/** video flags reported by bink_info_video() */
#define BINK_FLAG_ALPHA 0x00100000
#define BINK_FLAG_GRAY  0x00020000

#ifdef __cplusplus
extern "C" {
#endif

/* OPEN OPERATIONS */
/** open a Bink file from disk */
bink bink_open_file(const char *filename, unsigned char mode);
/** open a Bink file from an existing stream */
bink bink_open_filepointer(FILE *file, unsigned char mode);
/** open a Bink file from a memory buffer */
bink bink_open_memory(const unsigned char *buffer, unsigned long size);

/* CLOSE OPERATIONS */
/** close a Bink file and release all memory */
void bink_close(bink object);

/* GET FILE INFO OPERATIONS */
char bink_info_all(const bink object, unsigned long *frame,
                   unsigned long *frame_count, double *usf);
char bink_info_video(const bink object, unsigned long *w, unsigned long *h,
                     unsigned long *flags);
char bink_info_audio(const bink object, unsigned char *track_mask,
                     unsigned char channels[7], unsigned char bitdepth[7],
                     unsigned long audio_rate[7]);

/* ENABLE/DISABLE SWITCHES */
char bink_enable_video(bink object, unsigned char enable);
char bink_enable_audio(bink object, unsigned char track,
                       unsigned char enable);

/* FRAME OPERATIONS */
/** rewind to the first frame and decode it */
char bink_first(bink object);
/** advance to the next frame and decode it */
char bink_next(bink object);
/** seek to the first keyframe at or before N and decode it */
char bink_seek_keyframe(bink object, unsigned long frame);

/* FRAME DATA ACCESS */
/** retrieve the current frame as packed RGB24, converted on demand */
const unsigned char *bink_get_video(const bink object);
/** retrieve a decoded Y/U/V/A plane */
const unsigned char *bink_get_plane(const bink object, int plane);
/** retrieve the row stride in bytes of a decoded plane */
int bink_get_stride(const bink object, int plane);
/** retrieve decoded audio for a track (unimplemented in this version) */
const unsigned char *bink_get_audio(const bink object, unsigned char track);
/** retrieve the size of decoded audio for a track (0 in this version) */
unsigned long bink_get_audio_size(const bink object, unsigned char track);

#ifdef __cplusplus
}
#endif

#endif
