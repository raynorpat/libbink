#include "bink.h"
#include "bink_video.h"
#include "test_common.h"

static void w32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static unsigned long r32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void fill_header(unsigned char *buf, const char *sig,
                        unsigned long file_size_field)
{
    memset(buf, 0, 64);
    memcpy(buf, sig, 4);
    w32(buf + 4, file_size_field);
    w32(buf + 8, 1);
    w32(buf + 12, 8);
    w32(buf + 20, 32);
    w32(buf + 24, 32);
    w32(buf + 28, 1);
    w32(buf + 32, 1);
}

static int test_synthetic(void)
{
    unsigned char tiny[10];
    unsigned char buf[64];
    char status;
    bink b;

    memset(tiny, 0xA5, sizeof(tiny));
    b = bink_open_memory(tiny, sizeof(tiny));
    CHECK(b == NULL);

    fill_header(buf, "BIKb", 44);
    b = bink_open_memory(buf, 52);
    CHECK(b == NULL);

    fill_header(buf, "KB2a", 44);
    b = bink_open_memory(buf, 52);
    CHECK(b == NULL);

    /* Frame offset past both the declared file size and the buffer. */
    fill_header(buf, "BIKi", 44);
    w32(buf + 44, 200);
    b = bink_open_memory(buf, 52);
    CHECK(b == NULL);

    /* Header is accepted, but the frame packet is too short to decode. */
    fill_header(buf, "BIKi", 48);
    w32(buf + 44, 48);
    b = bink_open_memory(buf, 56);
    CHECK(b != NULL);
    CHECK(bink_enable_video(b, 1) == 0);
    status = bink_first(b);
    CHECK(status == BINK_ERROR);
    bink_close(b);
    return 0;
}

static int frame0_end(const unsigned char *data, unsigned long size,
                      unsigned long *end)
{
    unsigned long frames;
    unsigned long tracks;
    unsigned long pos;

    if (size < 48)
        return -1;
    frames = r32(data + 8);
    tracks = r32(data + 40);
    if (frames < 2 || tracks > 7)
        return -1;
    pos = 44;
    if (tracks > 0) {
        unsigned long extra = tracks * 12;

        if (extra > size - pos)
            return -1;
        pos += extra;
    }
    if (frames > (size - pos) / 4)
        return -1;
    *end = r32(data + pos + 4) & ~1UL;
    return 0;
}

/* One hash over the visible Y, U and V pixels of the current frame. */
static int hash_planes(bink b, unsigned long width, unsigned long height,
                       uint64_t *out)
{
    uint64_t hash = 0;
    int plane;

    for (plane = BINK_PLANE_Y; plane <= BINK_PLANE_V; plane++) {
        const unsigned char *p = bink_get_plane(b, plane);
        int w = plane == BINK_PLANE_Y ? (int)width : (int)((width + 1) / 2);
        int h = plane == BINK_PLANE_Y ? (int)height : (int)((height + 1) / 2);

        if (p == NULL)
            return -1;
        hash = hash * FNV_PRIME + fnv1a_rows(p, w, h, bink_get_stride(b, plane));
    }
    *out = hash;
    return 0;
}

static int test_truncated_media(void)
{
    const char *dir = test_media_dir();
    char path[1024];
    unsigned char *data = NULL;
    unsigned long size = 0;
    unsigned long end = 0;
    unsigned long trunc;
    unsigned long width = 0;
    unsigned long height = 0;
    unsigned long decoded = 0;
    uint64_t good = 0;
    char status;
    FILE *file;
    bink b;

    if (dir == NULL)
        return 0;

    snprintf(path, sizeof(path), "%s/logo_lucas.bik", dir);
    file = fopen(path, "rb");
    CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    CHECK(ftell(file) > 0);
    size = (unsigned long)ftell(file);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    data = (unsigned char *)malloc((size_t)size);
    CHECK(data != NULL);
    CHECK(fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);

    CHECK(frame0_end(data, size, &end) == 0);
    CHECK(end + 10 < size);
    trunc = end + 10;

    /* The untouched header describes a longer file, so opening the short
     * buffer is refused outright. */
    b = bink_open_memory(data, trunc);
    CHECK(b == NULL);

    /* Now cut inside frame 1's video: skip its audio packets and keep 64
     * bytes of picture, so the frame fails part-way through decoding. */
    {
        unsigned long tracks = r32(data + 40);
        unsigned long pos = end;
        unsigned long i;

        for (i = 0; i < tracks; i++) {
            CHECK(pos + 4 < size);
            pos += 4 + r32(data + pos);
        }
        trunc = pos + 64;
        CHECK(trunc < size);
    }

    /* Describe a two-frame file that ends there: frame 0 decodes and
     * frame 1 runs out of data. */
    w32(data + 4, trunc - 8);
    w32(data + 8, 2);
    w32(data + 12, trunc);
    b = bink_open_memory(data, trunc);
    free(data);
    CHECK(b != NULL);

    CHECK(bink_enable_video(b, 1) == 0);
    CHECK(bink_info_video(b, &width, &height, NULL) == 0);
    status = bink_first(b);
    while (status == BINK_MORE || status == BINK_LAST) {
        CHECK(hash_planes(b, width, height, &good) == 0);
        decoded++;
        status = bink_next(b);
    }
    CHECK(status == BINK_ERROR);
    /* A failed frame must not disturb the last one that decoded: the
     * planes still hold it, and so does the RGB view. */
    CHECK(decoded == 1);
    {
        uint64_t after = 0;

        CHECK(hash_planes(b, width, height, &after) == 0);
        CHECK(after == good);
        CHECK(bink_get_video(b) != NULL);
    }
    bink_close(b);
    return 0;
}

static int test_plane_padding(void)
{
    static const unsigned long sizes[][2] = {
        { 1920, 1080 },
        { 1440, 1080 },
        { 16, 8 }
    };
    size_t i;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        BinkVideo video;
        int y_blocks;
        int c_blocks;

        CHECK(bink_video_init(&video, sizes[i][0], sizes[i][1], 0, 'i') == 0);
        y_blocks = (video.bh + 1) & ~1;
        c_blocks = (video.cbh + 1) & ~1;
        CHECK(video.alloc_height[BINK_PLANE_Y] >= y_blocks * 8);
        CHECK(video.alloc_height[BINK_PLANE_U] >= c_blocks * 8);
        CHECK(video.alloc_height[BINK_PLANE_V] >= c_blocks * 8);
        CHECK(video.alloc_height[BINK_PLANE_A] >= y_blocks * 8);
        bink_video_uninit(&video);
    }
    return 0;
}

int main(void)
{
    if (test_synthetic() != 0)
        return 1;
    if (test_plane_padding() != 0)
        return 1;
    if (test_truncated_media() != 0)
        return 1;
    return 0;
}
