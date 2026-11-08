#include "bink.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t hash_plane(const unsigned char *plane, int width, int height,
                           int stride)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    int x;
    int y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            hash ^= plane[x];
            hash *= UINT64_C(1099511628211);
        }
        plane += stride;
    }
    return hash;
}

static int write_ppm(const char *outdir, unsigned long frame,
                     const unsigned char *rgb, unsigned long width,
                     unsigned long height)
{
    char path[1024];
    FILE *file;
    size_t size = (size_t)width * (size_t)height * 3;

    snprintf(path, sizeof(path), "%s/frame_%05lu.ppm", outdir, frame);
    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "bink_dump: cannot create %s\n", path);
        return -1;
    }
    if (fprintf(file, "P6\n%lu %lu\n255\n", width, height) < 0 ||
        fwrite(rgb, 1, size, file) != size) {
        fprintf(stderr, "bink_dump: cannot write %s\n", path);
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "bink_dump: cannot close %s\n", path);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *outdir = ".";
    unsigned long limit = 0;
    unsigned long frame_count = 0;
    unsigned long width = 0;
    unsigned long height = 0;
    unsigned long decoded = 0;
    double usf = 0.0;
    int i;
    int status;
    bink b;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            limit = strtoul(argv[++i], NULL, 10);
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr,
                    "usage: bink_dump [--frames N] file.bik [outdir]\n");
            return 2;
        } else if (path == NULL) {
            path = argv[i];
        } else {
            outdir = argv[i];
        }
    }
    if (path == NULL) {
        fprintf(stderr, "usage: bink_dump [--frames N] file.bik [outdir]\n");
        return 2;
    }

    b = bink_open_file(path, BINK_MODE_DISK);
    if (b == NULL) {
        fprintf(stderr, "bink_dump: cannot open %s\n", path);
        return 1;
    }

    if (bink_info_all(b, NULL, &frame_count, &usf) != 0 ||
        bink_info_video(b, &width, &height, NULL) != 0) {
        fprintf(stderr, "bink_dump: cannot read file info\n");
        bink_close(b);
        return 1;
    }
    printf("%s: %lux%lu, %lu frames, %.3f fps\n", path, width, height,
           frame_count, usf > 0.0 ? 1000000.0 / usf : 0.0);

    if (bink_enable_video(b, 1) != 0) {
        fprintf(stderr, "bink_dump: cannot enable video\n");
        bink_close(b);
        return 1;
    }

    status = bink_first(b);
    while (status == BINK_MORE || status == BINK_LAST) {
        const unsigned char *y;
        const unsigned char *u;
        const unsigned char *v;
        const unsigned char *rgb;
        unsigned long frame = 0;

        if (bink_info_all(b, &frame, NULL, NULL) != 0) {
            fprintf(stderr, "bink_dump: cannot read frame number\n");
            bink_close(b);
            return 1;
        }
        y = bink_get_plane(b, BINK_PLANE_Y);
        u = bink_get_plane(b, BINK_PLANE_U);
        v = bink_get_plane(b, BINK_PLANE_V);
        rgb = bink_get_video(b);
        if (y == NULL || u == NULL || v == NULL || rgb == NULL) {
            fprintf(stderr, "bink_dump: frame %lu is unavailable\n", frame);
            bink_close(b);
            return 1;
        }

        printf("frame %lu y=%" PRIu64 " u=%" PRIu64 " v=%" PRIu64 "\n",
               frame,
               hash_plane(y, (int)width, (int)height,
                          bink_get_stride(b, BINK_PLANE_Y)),
               hash_plane(u, (int)((width + 1) >> 1),
                          (int)((height + 1) >> 1),
                          bink_get_stride(b, BINK_PLANE_U)),
               hash_plane(v, (int)((width + 1) >> 1),
                          (int)((height + 1) >> 1),
                          bink_get_stride(b, BINK_PLANE_V)));

        if (write_ppm(outdir, frame, rgb, width, height) != 0) {
            bink_close(b);
            return 1;
        }
        decoded++;
        if (limit != 0 && decoded >= limit)
            break;
        if (status == BINK_LAST)
            break;
        status = bink_next(b);
        if (status == BINK_ERROR) {
            fprintf(stderr, "bink_dump: decode error at frame %lu\n",
                    frame + 1);
            bink_close(b);
            return 1;
        }
    }

    bink_close(b);
    return 0;
}
