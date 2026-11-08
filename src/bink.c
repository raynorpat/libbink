#include "bink_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const unsigned char *data;
    FILE *file;
    unsigned long size;
    unsigned long pos;
} BinkReader;

static int reader_read(BinkReader *r, void *dst, unsigned long n)
{
    if (n > r->size - r->pos)
        return -1;
    if (r->data) {
        memcpy(dst, r->data + r->pos, (size_t)n);
    } else if (fread(dst, 1, (size_t)n, r->file) != (size_t)n) {
        return -1;
    }
    r->pos += n;
    return 0;
}

static int reader_skip(BinkReader *r, unsigned long n)
{
    if (n > r->size - r->pos)
        return -1;
    if (!r->data && fseek(r->file, (long)n, SEEK_CUR) != 0)
        return -1;
    r->pos += n;
    return 0;
}

static int read_u16(BinkReader *r, unsigned int *out)
{
    unsigned char b[2];

    if (reader_read(r, b, 2) != 0)
        return -1;
    *out = (unsigned int)b[0] | ((unsigned int)b[1] << 8);
    return 0;
}

static int read_u32(BinkReader *r, unsigned long *out)
{
    unsigned char b[4];

    if (reader_read(r, b, 4) != 0)
        return -1;
    *out = (unsigned long)b[0] | ((unsigned long)b[1] << 8) |
           ((unsigned long)b[2] << 16) | ((unsigned long)b[3] << 24);
    return 0;
}

static void bink_free(bink b)
{
    if (b == NULL)
        return;
    if (b->owns_file && b->file)
        fclose(b->file);
    free(b->frame_offsets);
    free(b->frame_sizes);
    free(b->frame_keyframes);
    free(b->frame_data);
    free(b->mem);
    free(b);
}

static int bink_parse_header(bink b, BinkReader *r)
{
    unsigned char sig[4];
    unsigned long value;
    unsigned long header_end;
    unsigned long i;
    unsigned long *raw = NULL;
    int tracks;

    if (reader_read(r, sig, 4) != 0)
        return -1;

    if (memcmp(sig, "BIKf", 4) == 0) {
        b->version = 'f';
    } else if (memcmp(sig, "BIKg", 4) == 0) {
        b->version = 'g';
    } else if (memcmp(sig, "BIKh", 4) == 0) {
        b->version = 'h';
    } else if (memcmp(sig, "BIKi", 4) == 0) {
        b->version = 'i';
    } else if (memcmp(sig, "BIKb", 4) == 0 ||
               memcmp(sig, "BIKk", 4) == 0 ||
               (memcmp(sig, "KB2", 3) == 0)) {
        fputs("libbink:: unsupported Bink version\n", stderr);
        return -1;
    } else {
        fputs("libbink:: unrecognized file signature\n", stderr);
        return -1;
    }

    if (read_u32(r, &value) != 0)
        return -1;
    b->file_size = value + 8;
    if (b->file_size < 44 || b->file_size > r->size)
        return -1;

    if (read_u32(r, &b->frame_count) != 0)
        return -1;
    if (b->frame_count == 0 || b->frame_count > BINK_MAX_FRAMES)
        return -1;

    if (read_u32(r, &b->largest_frame) != 0)
        return -1;
    if (b->largest_frame > b->file_size)
        return -1;

    if (reader_skip(r, 4) != 0)
        return -1;

    if (read_u32(r, &b->width) != 0 || read_u32(r, &b->height) != 0)
        return -1;
    if (b->width == 0 || b->width > BINK_MAX_WIDTH ||
        b->height == 0 || b->height > BINK_MAX_HEIGHT)
        return -1;

    if (read_u32(r, &b->fps_num) != 0 || read_u32(r, &b->fps_den) != 0)
        return -1;
    if (b->fps_num == 0 || b->fps_den == 0)
        return -1;

    if (read_u32(r, &b->flags) != 0)
        return -1;

    if (read_u32(r, &value) != 0)
        return -1;
    if (value > BINK_MAX_AUDIO_TRACKS)
        return -1;
    tracks = (int)value;

    if (tracks > 0) {
        if (reader_skip(r, 4 * (unsigned long)tracks) != 0)
            return -1;
        for (i = 0; i < (unsigned long)tracks; i++) {
            unsigned int rate;
            unsigned int flags;

            if (read_u16(r, &rate) != 0 || read_u16(r, &flags) != 0)
                return -1;
            b->audio[i].rate = rate;
            b->audio[i].channels = (flags & 0x2000) ? 2 : 1;
            b->audio[i].bitdepth = 16;
            b->audio[i].flags = (unsigned short)flags;
        }
        if (reader_skip(r, 4 * (unsigned long)tracks) != 0)
            return -1;
    }
    b->audio_tracks = tracks;

    header_end = r->pos;
    if (b->frame_count > (b->file_size - header_end) / 4)
        return -1;

    raw = (unsigned long *)malloc((size_t)(b->frame_count * sizeof(*raw)));
    if (raw == NULL)
        return -1;
    for (i = 0; i < b->frame_count; i++) {
        if (read_u32(r, &raw[i]) != 0) {
            free(raw);
            return -1;
        }
    }

    b->frame_offsets = (unsigned long *)malloc(
        (size_t)(b->frame_count * sizeof(*b->frame_offsets)));
    b->frame_sizes = (unsigned long *)malloc(
        (size_t)(b->frame_count * sizeof(*b->frame_sizes)));
    b->frame_keyframes = (unsigned char *)malloc((size_t)b->frame_count);
    if (b->frame_offsets == NULL || b->frame_sizes == NULL ||
        b->frame_keyframes == NULL) {
        free(raw);
        return -1;
    }

    for (i = 0; i < b->frame_count; i++) {
        b->frame_offsets[i] = raw[i] & ~1UL;
        b->frame_keyframes[i] = (unsigned char)((i == 0) ? 1 : (raw[i] & 1));
        if (b->frame_offsets[i] < header_end ||
            b->frame_offsets[i] >= b->file_size) {
            free(raw);
            return -1;
        }
        if (i > 0 && b->frame_offsets[i] <= b->frame_offsets[i - 1]) {
            free(raw);
            return -1;
        }
    }
    for (i = 0; i + 1 < b->frame_count; i++)
        b->frame_sizes[i] = b->frame_offsets[i + 1] - b->frame_offsets[i];
    b->frame_sizes[b->frame_count - 1] =
        b->file_size - b->frame_offsets[b->frame_count - 1];

    free(raw);
    return 0;
}

static bink bink_open_stream(FILE *file, unsigned long size,
                             unsigned char mode)
{
    bink b = (bink)calloc(1, sizeof(*b));
    BinkReader reader;

    if (b == NULL)
        return NULL;

    b->file = file;
    b->mode = mode;
    b->owns_file = 1;

    memset(&reader, 0, sizeof(reader));
    reader.file = file;
    reader.size = size;

    if (bink_parse_header(b, &reader) != 0) {
        bink_free(b);
        return NULL;
    }
    return b;
}

bink bink_open_memory(const unsigned char *buffer, unsigned long size)
{
    bink b;
    BinkReader reader;

    if (buffer == NULL || size < 44)
        return NULL;

    b = (bink)calloc(1, sizeof(*b));
    if (b == NULL)
        return NULL;

    b->mem = (unsigned char *)malloc((size_t)size);
    if (b->mem == NULL) {
        free(b);
        return NULL;
    }
    memcpy(b->mem, buffer, (size_t)size);
    b->mem_size = size;
    b->mode = BINK_MODE_MEMORY;

    memset(&reader, 0, sizeof(reader));
    reader.data = b->mem;
    reader.size = size;

    if (bink_parse_header(b, &reader) != 0) {
        bink_free(b);
        return NULL;
    }
    return b;
}

bink bink_open_filepointer(FILE *file, unsigned char mode)
{
    long end;
    unsigned long size;
    bink b;

    if (file == NULL)
        return NULL;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    end = ftell(file);
    if (end < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    size = (unsigned long)end;

    if (mode == BINK_MODE_MEMORY) {
        unsigned char *buffer = (unsigned char *)malloc((size_t)size);

        if (buffer != NULL && size > 0) {
            if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
                free(buffer);
                buffer = NULL;
            }
        }
        fclose(file);
        if (buffer == NULL)
            return NULL;
        b = bink_open_memory(buffer, size);
        free(buffer);
        return b;
    }

    return bink_open_stream(file, size, BINK_MODE_DISK);
}

bink bink_open_file(const char *filename, unsigned char mode)
{
    FILE *file;

    if (filename == NULL)
        return NULL;

    file = fopen(filename, "rb");
    if (file == NULL)
        return NULL;

    return bink_open_filepointer(file, mode);
}

void bink_close(bink object)
{
    bink_free(object);
}

char bink_info_all(const bink object, unsigned long *frame,
                   unsigned long *frame_count, double *usf)
{
    if (object == NULL)
        return -1;
    if (frame)
        *frame = object->current_frame;
    if (frame_count)
        *frame_count = object->frame_count;
    if (usf)
        *usf = (double)object->fps_den * 1000000.0 /
               (double)object->fps_num;
    return 0;
}

char bink_info_video(const bink object, unsigned long *w, unsigned long *h,
                     unsigned long *flags)
{
    if (object == NULL)
        return -1;
    if (w)
        *w = object->width;
    if (h)
        *h = object->height;
    if (flags)
        *flags = object->flags;
    return 0;
}

char bink_info_audio(const bink object, unsigned char *track_mask,
                     unsigned char channels[7], unsigned char bitdepth[7],
                     unsigned long audio_rate[7])
{
    int i;

    if (object == NULL)
        return -1;

    if (track_mask)
        *track_mask = 0;
    for (i = 0; i < BINK_MAX_AUDIO_TRACKS; i++) {
        if (channels)
            channels[i] = 0;
        if (bitdepth)
            bitdepth[i] = 0;
        if (audio_rate)
            audio_rate[i] = 0;
    }
    for (i = 0; i < object->audio_tracks; i++) {
        if (track_mask)
            *track_mask |= (unsigned char)(1u << i);
        if (channels)
            channels[i] = object->audio[i].channels;
        if (bitdepth)
            bitdepth[i] = object->audio[i].bitdepth;
        if (audio_rate)
            audio_rate[i] = object->audio[i].rate;
    }
    return 0;
}

static unsigned long get_le32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int bink_read_frame_chunk(bink object, unsigned long index)
{
    unsigned long offset;
    unsigned long size;
    unsigned long pos = 0;
    int i;

    if (index >= object->frame_count)
        return -1;

    offset = object->frame_offsets[index];
    size = object->frame_sizes[index];

    if (size > object->frame_data_size) {
        unsigned char *data = (unsigned char *)realloc(object->frame_data,
                                                       (size_t)size);

        if (data == NULL)
            return -1;
        object->frame_data = data;
        object->frame_data_size = size;
    }

    if (object->mode == BINK_MODE_MEMORY) {
        memcpy(object->frame_data, object->mem + offset, (size_t)size);
    } else {
        if (fseek(object->file, (long)offset, SEEK_SET) != 0)
            return -1;
        if (fread(object->frame_data, 1, (size_t)size, object->file) !=
            (size_t)size)
            return -1;
    }

    for (i = 0; i < object->audio_tracks; i++) {
        unsigned long audio_size;

        if (size - pos < 4)
            return -1;
        audio_size = get_le32(object->frame_data + pos);
        if (audio_size > size - pos - 4)
            return -1;
        pos += 4 + audio_size;
    }

    object->video_data = object->frame_data + pos;
    object->video_size = size - pos;
    return 0;
}

char bink_enable_video(bink object, unsigned char enable)
{
    if (object == NULL)
        return -1;
    object->video_enabled = enable ? 1 : 0;
    return 0;
}

char bink_enable_audio(bink object, unsigned char track, unsigned char enable)
{
    (void)track;
    (void)enable;

    if (object == NULL)
        return -1;
    return 0;
}

char bink_first(bink object)
{
    if (object == NULL)
        return BINK_ERROR;
    if (bink_read_frame_chunk(object, 0) != 0)
        return BINK_ERROR;
    object->current_frame = 0;
    return (object->frame_count == 1) ? BINK_LAST : BINK_MORE;
}

char bink_next(bink object)
{
    unsigned long next;

    if (object == NULL)
        return BINK_ERROR;
    if (object->current_frame + 1 >= object->frame_count)
        return BINK_DONE;

    next = object->current_frame + 1;
    if (bink_read_frame_chunk(object, next) != 0)
        return BINK_ERROR;
    object->current_frame = next;
    return (next + 1 == object->frame_count) ? BINK_LAST : BINK_MORE;
}

char bink_seek_keyframe(bink object, unsigned long frame)
{
    if (object == NULL)
        return -1;
    if (frame >= object->frame_count)
        frame = object->frame_count - 1;
    while (frame > 0 && !object->frame_keyframes[frame])
        frame--;
    if (bink_read_frame_chunk(object, frame) != 0)
        return -1;
    object->current_frame = frame;
    return 0;
}
