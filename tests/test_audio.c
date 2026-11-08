#include "bink.h"
#include "bink_internal.h"
#include "reference_audio.h"
#include "test_common.h"

static void w16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void w32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void fill_one_track(unsigned char *buf)
{
    memset(buf, 0, 68);
    memcpy(buf, "BIKi", 4);
    w32(buf + 4, 60);          /* file size field: buffer length minus 8 */
    w32(buf + 8, 1);           /* one frame */
    w32(buf + 12, 8);          /* largest frame */
    w32(buf + 20, 64);         /* width */
    w32(buf + 24, 64);         /* height */
    w32(buf + 28, 30);         /* fps numerator */
    w32(buf + 32, 1);          /* fps denominator */
    w32(buf + 40, 1);          /* one audio track */
    w32(buf + 44, 134400);     /* maximum decoded size */
    w16(buf + 48, 44100);      /* sample rate */
    w16(buf + 50, 0x3000);     /* DCT and stereo */
    w32(buf + 52, 0);          /* track id */
    w32(buf + 56, 60);         /* frame 0 offset */
    w32(buf + 60, 4);          /* 4-byte audio chunk, empty video tail */
}

static int test_synthetic_max_decoded(void)
{
    unsigned char buf[68];
    bink b;

    fill_one_track(buf);
    b = bink_open_memory(buf, sizeof(buf));
    CHECK(b != NULL);
    CHECK(b->audio_tracks == 1);
    CHECK(b->audio[0].max_decoded == 134400);
    bink_close(b);
    return 0;
}

static int test_enable_without_pcm(void)
{
    unsigned char buf[68];
    char status;
    bink b;

    CHECK(bink_enable_audio(NULL, 0, 1) == -1);

    fill_one_track(buf);
    b = bink_open_memory(buf, sizeof(buf));
    CHECK(b != NULL);
    CHECK(bink_enable_audio(b, 1, 1) == -1);
    CHECK(bink_enable_audio(b, 0, 1) == 0);
    CHECK(bink_get_audio(b, 0) == NULL);
    CHECK(bink_get_audio_size(b, 0) == 0);

    status = bink_first(b);
    CHECK(status == BINK_MORE || status == BINK_LAST);
    CHECK(bink_get_audio(b, 0) == NULL);
    CHECK(bink_get_audio_size(b, 0) == 0);

    CHECK(bink_enable_audio(b, 0, 0) == 0);
    status = bink_first(b);
    CHECK(status != BINK_ERROR);
    bink_close(b);
    return 0;
}

static int expect_max_decoded(const char *name, int tracks,
                              unsigned long max_decoded)
{
    const char *dir = test_media_dir();
    char path[1024];
    bink b;

    if (dir == NULL)
        return 0;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    b = bink_open_file(path, BINK_MODE_DISK);
    CHECK(b != NULL);
    CHECK(b->audio_tracks == tracks);
    if (tracks > 0)
        CHECK(b->audio[0].max_decoded == max_decoded);
    bink_close(b);
    return 0;
}

static int test_real_max_decoded(void)
{
    if (test_media_dir() == NULL)
        return 0;
    if (expect_max_decoded("logo_lucas.bik", 1, 134400) != 0)
        return 1;
    if (expect_max_decoded("valve.bik", 1, 138240) != 0)
        return 1;
    if (expect_max_decoded("original.bik", 1, 138240) != 0)
        return 1;
    if (expect_max_decoded("ATI-9700-Animusic-Movie-v1.0.bik", 1, 145920) != 0)
        return 1;
    if (expect_max_decoded("Robotica_1080_bink_4100k.bik", 0, 0) != 0)
        return 1;

    {
        char path[1024];
        bink b;

        snprintf(path, sizeof(path), "%s/Robotica_1080_bink_4100k.bik",
                 test_media_dir());
        b = bink_open_file(path, BINK_MODE_DISK);
        CHECK(b != NULL);
        CHECK(bink_enable_audio(b, 0, 1) == -1);
        bink_close(b);
    }
    return 0;
}

static const struct reference_audio *find_audio_ref(const char *file)
{
    size_t i;

    for (i = 0; i < sizeof(reference_audio) / sizeof(reference_audio[0]); i++) {
        if (strcmp(reference_audio[i].file, file) == 0)
            return &reference_audio[i];
    }
    return NULL;
}

static int walk_dct_file(const char *file, int enable_video)
{
    const char *dir = test_media_dir();
    const struct reference_audio *ref = NULL;
    char path[1024];
    unsigned char *stream = NULL;
    unsigned long got = 0;
    unsigned long frames = 0;
    unsigned long frame = 0;
    int saw_empty = 0;
    char status;
    bink b;

    if (dir == NULL)
        return 0;

    ref = find_audio_ref(file);
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    b = bink_open_file(path, BINK_MODE_DISK);
    CHECK(b != NULL);
    CHECK(bink_info_all(b, NULL, &frames, NULL) == 0);
    CHECK(bink_enable_audio(b, 0, 1) == 0);
    if (enable_video)
        CHECK(bink_enable_video(b, 1) == 0);

    status = bink_first(b);
    while (status == BINK_MORE || status == BINK_LAST) {
        unsigned long size = bink_get_audio_size(b, 0);
        const unsigned char *pcm = bink_get_audio(b, 0);

        if (size == 0) {
            CHECK(pcm == NULL);
            if (frame > 0)
                saw_empty = 1;
        } else {
            unsigned char *grown;

            CHECK(pcm != NULL);
            grown = (unsigned char *)realloc(stream, (size_t)got + (size_t)size);
            CHECK(grown != NULL);
            stream = grown;
            memcpy(stream + got, pcm, (size_t)size);
            got += size;
        }

        if (frame == 0 && ref != NULL) {
            CHECK(size == ref->frame0_bytes);
            CHECK(fnv1a(pcm, (size_t)size) == ref->frame0_hash);
        }
        if (frame == 0 &&
            strcmp(file, "ATI-9700-Animusic-Movie-v1.0.bik") == 0)
            CHECK(size == 145920);
        if (ref != NULL && frame == frames / 2) {
            CHECK(got > ref->frame0_bytes);
            CHECK(got < ref->pcm_bytes);
        }

        if (status == BINK_LAST)
            break;
        status = bink_next(b);
        frame++;
    }
    CHECK(status != BINK_ERROR);

    if (ref != NULL) {
        CHECK(saw_empty);
        CHECK(got == ref->pcm_bytes);
        CHECK(fnv1a(stream, (size_t)got) == ref->hash);
    }
    free(stream);
    bink_close(b);
    return 0;
}

static int test_dct_matches_reference(void)
{
    if (test_media_dir() == NULL)
        return 0;
    if (walk_dct_file("logo_lucas.bik", 1) != 0)
        return 1;
    if (walk_dct_file("valve.bik", 1) != 0)
        return 1;
    if (walk_dct_file("original.bik", 1) != 0)
        return 1;
    if (walk_dct_file("ATI-9700-Animusic-Movie-v1.0.bik", 0) != 0)
        return 1;
    return 0;
}

static int read_logo_frame0_audio(unsigned char **payload,
                                  unsigned long *payload_size,
                                  unsigned long *rate,
                                  unsigned short *flags)
{
    char path[1024];
    unsigned char hdr[4];
    unsigned char *data;
    unsigned long audio_size;
    bink b;

    snprintf(path, sizeof(path), "%s/logo_lucas.bik", test_media_dir());
    b = bink_open_file(path, BINK_MODE_DISK);
    CHECK(b != NULL);
    CHECK(b->audio_tracks == 1);
    CHECK(b->frame_sizes[0] >= 4);
    CHECK(fseek(b->file, (long)b->frame_offsets[0], SEEK_SET) == 0);
    CHECK(fread(hdr, 1, 4, b->file) == 4);
    audio_size = (unsigned long)hdr[0] | ((unsigned long)hdr[1] << 8) |
                 ((unsigned long)hdr[2] << 16) | ((unsigned long)hdr[3] << 24);
    CHECK(audio_size > 8);
    CHECK(audio_size + 4 <= b->frame_sizes[0]);
    data = (unsigned char *)malloc((size_t)audio_size);
    CHECK(data != NULL);
    CHECK(fread(data, 1, (size_t)audio_size, b->file) == (size_t)audio_size);
    *payload = data;
    *payload_size = audio_size;
    *rate = b->audio[0].rate;
    *flags = b->audio[0].flags;
    bink_close(b);
    return 0;
}

/* One-frame BIKi. Header matches bink_parse_header: max-decoded block,
 * then rate/flags, then track ids, then the frame index. Video tail empty. */
static unsigned char *build_one_frame(int tracks,
                                      const unsigned long *max_decoded,
                                      const unsigned long *rate,
                                      const unsigned short *aflags,
                                      const unsigned char *const *payloads,
                                      const unsigned long *payload_sizes,
                                      unsigned long *out_size)
{
    unsigned long header = 44 + 12UL * (unsigned long)tracks + 4;
    unsigned long packet = 0;
    unsigned long total;
    unsigned long pos;
    unsigned char *buf;
    int i;

    for (i = 0; i < tracks; i++)
        packet += 4 + payload_sizes[i];
    total = header + packet;
    buf = (unsigned char *)calloc(1, (size_t)total);
    if (buf == NULL)
        return NULL;

    memcpy(buf, "BIKi", 4);
    w32(buf + 4, total - 8);
    w32(buf + 8, 1);
    w32(buf + 12, packet);
    w32(buf + 20, 64);
    w32(buf + 24, 64);
    w32(buf + 28, 30);
    w32(buf + 32, 1);
    w32(buf + 40, (unsigned long)tracks);
    pos = 44;
    for (i = 0; i < tracks; i++) {
        w32(buf + pos, max_decoded[i]);
        pos += 4;
    }
    for (i = 0; i < tracks; i++) {
        w16(buf + pos, (unsigned int)rate[i]);
        w16(buf + pos + 2, aflags[i]);
        pos += 4;
    }
    pos += 4UL * (unsigned long)tracks;
    w32(buf + pos, header);
    pos += 4;
    for (i = 0; i < tracks; i++) {
        w32(buf + pos, payload_sizes[i]);
        pos += 4;
        if (payload_sizes[i] > 0)
            memcpy(buf + pos, payloads[i], (size_t)payload_sizes[i]);
        pos += payload_sizes[i];
    }
    *out_size = total;
    return buf;
}

static int test_truncated_payload(void)
{
    unsigned char *payload = NULL;
    unsigned char *cut = NULL;
    unsigned char *file = NULL;
    unsigned long payload_size = 0;
    unsigned long rate = 0;
    unsigned short flags = 0;
    unsigned long file_size = 0;
    const unsigned char *ptrs[1];
    unsigned long sizes[1];
    unsigned long max_decoded[1];
    unsigned long rates[1];
    unsigned short aflags[1];
    char status;
    bink b;

    if (test_media_dir() == NULL)
        return 0;
    if (read_logo_frame0_audio(&payload, &payload_size, &rate, &flags) != 0)
        return 1;

    cut = (unsigned char *)malloc((size_t)payload_size - 8);
    CHECK(cut != NULL);
    memcpy(cut, payload, (size_t)payload_size - 8);
    ptrs[0] = cut;
    sizes[0] = payload_size - 8;
    max_decoded[0] = 134400;
    rates[0] = rate;
    aflags[0] = flags;
    file = build_one_frame(1, max_decoded, rates, aflags, ptrs, sizes,
                           &file_size);
    CHECK(file != NULL);

    b = bink_open_memory(file, file_size);
    CHECK(b != NULL);
    CHECK(bink_enable_audio(b, 0, 1) == 0);
    status = bink_first(b);
    CHECK(status != BINK_ERROR);
    CHECK(bink_get_audio_size(b, 0) == 0);
    CHECK(bink_get_audio(b, 0) == NULL);
    bink_close(b);
    free(file);
    free(cut);
    free(payload);
    return 0;
}

static int test_second_track_isolated(void)
{
    const struct reference_audio *ref = find_audio_ref("logo_lucas.bik");
    unsigned char *payload = NULL;
    unsigned char zeros[8];
    unsigned char *file = NULL;
    unsigned long payload_size = 0;
    unsigned long rate = 0;
    unsigned short flags = 0;
    unsigned long file_size = 0;
    const unsigned char *ptrs[2];
    unsigned long sizes[2];
    unsigned long max_decoded[2];
    unsigned long rates[2];
    unsigned short aflags[2];
    const unsigned char *pcm;
    unsigned long size;
    char status;
    bink b;

    if (test_media_dir() == NULL)
        return 0;
    CHECK(ref != NULL);
    if (read_logo_frame0_audio(&payload, &payload_size, &rate, &flags) != 0)
        return 1;

    memset(zeros, 0, sizeof(zeros));
    ptrs[0] = payload;
    ptrs[1] = zeros;
    sizes[0] = payload_size;
    sizes[1] = 8;
    max_decoded[0] = 134400;
    max_decoded[1] = 134400;
    rates[0] = rates[1] = rate;
    aflags[0] = aflags[1] = flags;
    file = build_one_frame(2, max_decoded, rates, aflags, ptrs, sizes,
                           &file_size);
    CHECK(file != NULL);

    b = bink_open_memory(file, file_size);
    CHECK(b != NULL);
    CHECK(bink_enable_audio(b, 0, 1) == 0);
    CHECK(bink_enable_audio(b, 1, 1) == 0);
    status = bink_first(b);
    CHECK(status != BINK_ERROR);
    size = bink_get_audio_size(b, 0);
    pcm = bink_get_audio(b, 0);
    CHECK(size == 134400);
    CHECK(pcm != NULL);
    CHECK(fnv1a(pcm, (size_t)size) == ref->frame0_hash);
    CHECK(bink_get_audio_size(b, 1) == 0);
    CHECK(bink_get_audio(b, 1) == NULL);
    bink_close(b);
    free(file);
    free(payload);
    return 0;
}

static int collect_track0(bink b, unsigned char **stream, unsigned long *got)
{
    char status = bink_first(b);

    *stream = NULL;
    *got = 0;
    while (status == BINK_MORE || status == BINK_LAST) {
        unsigned long size = bink_get_audio_size(b, 0);
        const unsigned char *pcm = bink_get_audio(b, 0);

        if (size == 0) {
            CHECK(pcm == NULL);
        } else {
            unsigned char *grown = (unsigned char *)realloc(
                *stream, (size_t)*got + (size_t)size);

            CHECK(grown != NULL);
            *stream = grown;
            memcpy(*stream + *got, pcm, (size_t)size);
            *got += size;
        }
        if (status == BINK_LAST)
            break;
        status = bink_next(b);
    }
    CHECK(status != BINK_ERROR);
    return 0;
}

static int test_seek_and_modes(void)
{
    const struct reference_audio *ref = find_audio_ref("logo_lucas.bik");
    char path[1024];
    const unsigned char *pcm;
    unsigned char *disk_pcm = NULL;
    unsigned char *mem_pcm = NULL;
    unsigned long size;
    unsigned long disk_bytes = 0;
    unsigned long mem_bytes = 0;
    uint64_t fresh_hash;
    uint64_t disk_hash;
    uint64_t mem_hash;
    char status;
    int i;
    bink b;

    if (test_media_dir() == NULL)
        return 0;
    CHECK(ref != NULL);
    snprintf(path, sizeof(path), "%s/logo_lucas.bik", test_media_dir());

    b = bink_open_file(path, BINK_MODE_DISK);
    CHECK(b != NULL);
    CHECK(bink_enable_audio(b, 0, 1) == 0);
    CHECK(bink_enable_video(b, 1) == 0);
    status = bink_first(b);
    CHECK(status == BINK_MORE);
    size = bink_get_audio_size(b, 0);
    pcm = bink_get_audio(b, 0);
    CHECK(size == ref->frame0_bytes);
    CHECK(pcm != NULL);
    fresh_hash = fnv1a(pcm, (size_t)size);
    for (i = 0; i < 10; i++) {
        status = bink_next(b);
        CHECK(status == BINK_MORE);
    }
    CHECK(bink_seek_keyframe(b, 10) == 0);
    size = bink_get_audio_size(b, 0);
    pcm = bink_get_audio(b, 0);
    CHECK(size == ref->frame0_bytes);
    CHECK(pcm != NULL);
    CHECK(fnv1a(pcm, (size_t)size) == fresh_hash);
    bink_close(b);

    b = bink_open_file(path, BINK_MODE_DISK);
    CHECK(b != NULL);
    CHECK(bink_enable_audio(b, 0, 1) == 0);
    CHECK(bink_enable_video(b, 1) == 0);
    if (collect_track0(b, &disk_pcm, &disk_bytes) != 0) {
        free(disk_pcm);
        bink_close(b);
        return 1;
    }
    bink_close(b);

    b = bink_open_file(path, BINK_MODE_MEMORY);
    CHECK(b != NULL);
    CHECK(bink_enable_audio(b, 0, 1) == 0);
    CHECK(bink_enable_video(b, 1) == 0);
    if (collect_track0(b, &mem_pcm, &mem_bytes) != 0) {
        free(disk_pcm);
        free(mem_pcm);
        bink_close(b);
        return 1;
    }
    bink_close(b);

    CHECK(disk_bytes == ref->pcm_bytes);
    CHECK(mem_bytes == disk_bytes);
    disk_hash = fnv1a(disk_pcm, (size_t)disk_bytes);
    mem_hash = fnv1a(mem_pcm, (size_t)mem_bytes);
    CHECK(disk_hash == ref->hash);
    CHECK(mem_hash == disk_hash);
    free(disk_pcm);
    free(mem_pcm);
    return 0;
}

int main(void)
{
    if (test_synthetic_max_decoded() != 0)
        return 1;
    if (test_enable_without_pcm() != 0)
        return 1;
    if (test_real_max_decoded() != 0)
        return 1;
    if (test_dct_matches_reference() != 0)
        return 1;
    if (test_truncated_payload() != 0)
        return 1;
    if (test_second_track_isolated() != 0)
        return 1;
    if (test_seek_and_modes() != 0)
        return 1;
    return 0;
}
