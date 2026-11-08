#include "bink.h"
#include "test_common.h"

static unsigned char clamp_byte(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (unsigned char)value;
}

static int test_rgb(void)
{
    const char *dir = test_media_dir();
    char path[1024];
    const unsigned char *rgb;
    const unsigned char *rgb2;
    const unsigned char *y_plane;
    const unsigned char *u_plane;
    const unsigned char *v_plane;
    unsigned long width = 0;
    unsigned long height = 0;
    int y_stride;
    int u_stride;
    int v_stride;
    int sx[8] = {0, 1, 17, 63, 128, 200, 319, 400};
    int sy[8] = {0, 3, 42, 99, 160, 240, 300, 400};
    int i;
    bink b;

    if (dir == NULL)
        return 77;

    snprintf(path, sizeof(path), "%s/logo_lucas.bik", dir);
    b = bink_open_file(path, BINK_MODE_DISK);
    CHECK(b != NULL);
    CHECK(bink_enable_video(b, 1) == 0);

    CHECK(bink_get_plane(b, BINK_PLANE_Y) == NULL);
    CHECK(bink_get_video(b) == NULL);
    CHECK(bink_get_stride(b, BINK_PLANE_Y) == 0);

    {
        char status = bink_first(b);

        CHECK(status == BINK_MORE || status == BINK_LAST);
    }
    CHECK(bink_info_video(b, &width, &height, NULL) == 0);

    y_plane = bink_get_plane(b, BINK_PLANE_Y);
    u_plane = bink_get_plane(b, BINK_PLANE_U);
    v_plane = bink_get_plane(b, BINK_PLANE_V);
    y_stride = bink_get_stride(b, BINK_PLANE_Y);
    u_stride = bink_get_stride(b, BINK_PLANE_U);
    v_stride = bink_get_stride(b, BINK_PLANE_V);
    CHECK(y_plane != NULL && u_plane != NULL && v_plane != NULL);
    CHECK(y_stride > 0 && u_stride > 0 && v_stride > 0);

    rgb = bink_get_video(b);
    CHECK(rgb != NULL);
    for (i = 0; i < 8; i++) {
        int x = sx[i];
        int y = sy[i];
        int c = (int)y_plane[y * y_stride + x] - 16;
        int d = (int)u_plane[(y >> 1) * u_stride + (x >> 1)] - 128;
        int e = (int)v_plane[(y >> 1) * v_stride + (x >> 1)] - 128;
        unsigned char er = clamp_byte((298 * c + 409 * e + 128) >> 8);
        unsigned char eg = clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8);
        unsigned char eb = clamp_byte((298 * c + 516 * d + 128) >> 8);
        size_t off = ((size_t)y * width + (size_t)x) * 3;

        CHECK(rgb[off + 0] == er);
        CHECK(rgb[off + 1] == eg);
        CHECK(rgb[off + 2] == eb);
    }

    rgb2 = bink_get_video(b);
    CHECK(rgb2 == rgb);
    bink_close(b);

    /* logo_lucas holds several identical frames; use another file to verify
     * that advancing invalidates the cached RGB buffer. */
    snprintf(path, sizeof(path), "%s/original.bik", dir);
    b = bink_open_file(path, BINK_MODE_DISK);
    CHECK(b != NULL);
    CHECK(bink_enable_video(b, 1) == 0);
    CHECK(bink_first(b) == BINK_MORE || bink_first(b) == BINK_LAST);
    CHECK(bink_info_video(b, &width, &height, NULL) == 0);
    rgb = bink_get_video(b);
    CHECK(rgb != NULL);
    {
        uint64_t before = fnv1a(rgb, (size_t)width * height * 3);

        CHECK(bink_next(b) == BINK_MORE);
        rgb2 = bink_get_video(b);
        CHECK(rgb2 != NULL);
        CHECK(fnv1a(rgb2, (size_t)width * height * 3) != before);
    }

    bink_close(b);
    return 0;
}

int main(void)
{
    int ret = test_rgb();

    if (ret == 77) {
        fprintf(stderr, "SKIP: BINK_TEST_MEDIA not set\n");
        return 77;
    }
    return ret;
}
