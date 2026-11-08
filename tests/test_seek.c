#include "bink.h"
#include "test_common.h"

static int decode_to(bink b, unsigned long target)
{
    unsigned long cur = 0;
    char status;

    if (target == 0) {
        status = bink_first(b);
        return (status == BINK_MORE || status == BINK_LAST) ? 0 : -1;
    }
    if (bink_seek_keyframe(b, target) != 0)
        return -1;
    if (bink_info_all(b, &cur, NULL, NULL) != 0)
        return -1;
    while (cur < target) {
        status = bink_next(b);
        if (status == BINK_ERROR || status == BINK_DONE)
            return -1;
        if (bink_info_all(b, &cur, NULL, NULL) != 0)
            return -1;
    }
    return 0;
}

static int test_seek(void)
{
    const char *dir = test_media_dir();
    char path[1024];
    unsigned long cur = 0;
    unsigned long frames = 0;
    bink b;

    if (dir == NULL)
        return 77;

    snprintf(path, sizeof(path), "%s/logo_lucas.bik", dir);
    b = bink_open_file(path, BINK_MODE_DISK);
    CHECK(b != NULL);
    CHECK(bink_enable_video(b, 1) == 0);
    CHECK(bink_info_all(b, NULL, &frames, NULL) == 0);

    CHECK(decode_to(b, 100) == 0);
    CHECK(bink_info_all(b, &cur, NULL, NULL) == 0 && cur == 100);

    CHECK(bink_seek_keyframe(b, 100) == 0);
    CHECK(bink_info_all(b, &cur, NULL, NULL) == 0 && cur == 0);
    CHECK(bink_next(b) == BINK_MORE);
    CHECK(bink_info_all(b, &cur, NULL, NULL) == 0 && cur == 1);

    CHECK(bink_seek_keyframe(b, frames + 500) == 0);
    CHECK(bink_info_all(b, &cur, NULL, NULL) == 0 && cur == 0);

    bink_close(b);

    b = bink_open_file(path, BINK_MODE_DISK);
    CHECK(b != NULL);
    CHECK(bink_enable_video(b, 1) == 0);
    CHECK(decode_to(b, 3) == 0);
    CHECK(bink_seek_keyframe(b, 1) == 0);
    CHECK(bink_info_all(b, &cur, NULL, NULL) == 0 && cur == 0);
    CHECK(bink_get_plane(b, BINK_PLANE_Y) != NULL);
    bink_close(b);
    return 0;
}

int main(void)
{
    int ret = test_seek();

    if (ret == 77) {
        fprintf(stderr, "SKIP: BINK_TEST_MEDIA not set\n");
        return 77;
    }
    return ret;
}
