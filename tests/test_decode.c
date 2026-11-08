#include "bink.h"
#include "bink_internal.h"
#include "bink_video.h"
#include "reference_checksums.h"
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

static int check_references(void)
{
    const char *dir = test_media_dir();
    unsigned long i;
    char path[1024];

    for (i = 0; i < reference_frame_count; i++) {
        const struct reference_frame *ref = &reference_frames[i];
        BinkVideo *video;
        bink b;
        uint64_t y, u, v;
        int pw, ph, stride;

        snprintf(path, sizeof(path), "%s/%s", dir, ref->file);
        b = bink_open_file(path, BINK_MODE_DISK);
        CHECK(b != NULL);
        CHECK(bink_enable_video(b, 1) == 0);
        CHECK(decode_to(b, ref->frame) == 0);
        video = b->video;
        CHECK(video != NULL);

        pw = bink_video_plane_width(video, BINK_PLANE_Y);
        ph = bink_video_plane_height(video, BINK_PLANE_Y);
        stride = bink_video_stride(video, BINK_PLANE_Y);
        y = fnv1a_rows(bink_video_plane(video, BINK_PLANE_Y), pw, ph, stride);
        pw = bink_video_plane_width(video, BINK_PLANE_U);
        ph = bink_video_plane_height(video, BINK_PLANE_U);
        stride = bink_video_stride(video, BINK_PLANE_U);
        u = fnv1a_rows(bink_video_plane(video, BINK_PLANE_U), pw, ph, stride);
        pw = bink_video_plane_width(video, BINK_PLANE_V);
        ph = bink_video_plane_height(video, BINK_PLANE_V);
        stride = bink_video_stride(video, BINK_PLANE_V);
        v = fnv1a_rows(bink_video_plane(video, BINK_PLANE_V), pw, ph, stride);

        if (y != ref->y_hash || u != ref->u_hash || v != ref->v_hash) {
            fprintf(stderr,
                    "MISMATCH %s frame %lu: got %016llx/%016llx/%016llx "
                    "want %016llx/%016llx/%016llx\n",
                    ref->file, ref->frame,
                    (unsigned long long)y, (unsigned long long)u,
                    (unsigned long long)v,
                    (unsigned long long)ref->y_hash,
                    (unsigned long long)ref->u_hash,
                    (unsigned long long)ref->v_hash);
            bink_close(b);
            return 1;
        }
        bink_close(b);
    }
    return 0;
}

static int check_full_walk(void)
{
    static const char *files[] = {
        "ATI-9700-Animusic-Movie-v1.0.bik",
        "Robotica_1080_bink_4100k.bik",
        "logo_lucas.bik",
        "original.bik",
        "valve.bik",
    };
    const char *dir = test_media_dir();
    size_t f;
    char path[1024];

    for (f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
        unsigned long frames = 0;
        unsigned long count = 1;
        char status;
        bink b;

        snprintf(path, sizeof(path), "%s/%s", dir, files[f]);
        b = bink_open_file(path, BINK_MODE_DISK);
        CHECK(b != NULL);
        CHECK(bink_info_all(b, NULL, &frames, NULL) == 0);
        CHECK(bink_enable_video(b, 1) == 0);
        status = bink_first(b);
        CHECK(status == BINK_MORE || status == BINK_LAST);
        while ((status = bink_next(b)) == BINK_MORE ||
               status == BINK_LAST) {
            count++;
            if (status == BINK_LAST)
                break;
        }
        if (status == BINK_ERROR) {
            fprintf(stderr, "decode error in %s at frame %lu\n",
                    files[f], count - 1);
            bink_close(b);
            return 1;
        }
        if (count != frames) {
            fprintf(stderr, "%s: decoded %lu of %lu frames\n",
                    files[f], count, frames);
            bink_close(b);
            return 1;
        }
        bink_close(b);
    }
    return 0;
}

int main(void)
{
    if (test_media_dir() == NULL) {
        fprintf(stderr, "SKIP: BINK_TEST_MEDIA not set\n");
        return 77;
    }
    if (check_references() != 0)
        return 1;
    if (check_full_walk() != 0)
        return 1;
    return 0;
}
