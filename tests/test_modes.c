#include "bink.h"
#include "test_common.h"

static int hash_planes(bink b, uint64_t out[3])
{
    unsigned long width = 0;
    unsigned long height = 0;
    int planes[3] = { BINK_PLANE_Y, BINK_PLANE_U, BINK_PLANE_V };
    int i;

    CHECK(bink_info_video(b, &width, &height, NULL) == 0);
    for (i = 0; i < 3; i++) {
        int pw = (i == 0) ? (int)width : (int)((width + 1) >> 1);
        int ph = (i == 0) ? (int)height : (int)((height + 1) >> 1);
        const unsigned char *plane = bink_get_plane(b, planes[i]);
        int stride = bink_get_stride(b, planes[i]);

        CHECK(plane != NULL);
        CHECK(stride >= pw);
        out[i] = fnv1a_rows(plane, pw, ph, stride);
    }
    return 0;
}

static int advance(bink object)
{
    char status = bink_next(object);

    return (status == BINK_MORE || status == BINK_LAST) ? 0 : -1;
}

static int test_parity(void)
{
    const char *dir = test_media_dir();
    char path[1024];
    bink disk;
    bink memory;
    char status;
    int frame;

    if (dir == NULL)
        return 77;

    snprintf(path, sizeof(path), "%s/logo_lucas.bik", dir);
    disk = bink_open_file(path, BINK_MODE_DISK);
    memory = bink_open_file(path, BINK_MODE_MEMORY);
    CHECK(disk != NULL && memory != NULL);
    CHECK(bink_enable_video(disk, 1) == 0);
    CHECK(bink_enable_video(memory, 1) == 0);

    status = bink_first(disk);
    CHECK(status == BINK_MORE || status == BINK_LAST);
    status = bink_first(memory);
    CHECK(status == BINK_MORE || status == BINK_LAST);

    for (frame = 0; frame < 3; frame++) {
        uint64_t disk_hash[3];
        uint64_t memory_hash[3];

        CHECK(hash_planes(disk, disk_hash) == 0);
        CHECK(hash_planes(memory, memory_hash) == 0);
        CHECK(disk_hash[0] == memory_hash[0]);
        CHECK(disk_hash[1] == memory_hash[1]);
        CHECK(disk_hash[2] == memory_hash[2]);
        if (frame == 2)
            break;
        CHECK(advance(disk) == 0);
        CHECK(advance(memory) == 0);
    }

    bink_close(disk);
    bink_close(memory);
    return 0;
}

int main(void)
{
    int ret = test_parity();

    if (ret == 77) {
        fprintf(stderr, "SKIP: BINK_TEST_MEDIA not set\n");
        return 77;
    }
    return ret;
}
