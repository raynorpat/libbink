#ifndef LIBBINK_TEST_COMMON_H
#define LIBBINK_TEST_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

#define FNV_OFFSET UINT64_C(14695981039346656037)
#define FNV_PRIME  UINT64_C(1099511628211)

static inline uint64_t fnv1a(const void *data, size_t size)
{
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = FNV_OFFSET;
    size_t i;

    for (i = 0; i < size; i++) {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}

static inline uint64_t fnv1a_rows(const unsigned char *p, int width,
                                  int height, int stride)
{
    uint64_t h = FNV_OFFSET;
    int x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            h ^= p[x];
            h *= FNV_PRIME;
        }
        p += stride;
    }
    return h;
}

static inline const char *test_media_dir(void)
{
    return getenv("BINK_TEST_MEDIA");
}

#endif
