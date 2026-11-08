#include "tools/bink_mix.h"
#include "test_common.h"

static int test_upsample(void)
{
    static const int16_t in[] = {0, 1000};
    int16_t out[4];
    BinkMixTrack track;

    memset(&track, 0, sizeof(track));
    track.pcm = in;
    track.samples = 2;
    track.rate = 1000;
    track.channels = 1;
    CHECK(bink_mix(&track, 1, 2000, 1, out, 4) == 0);
    CHECK(out[0] == 0);
    CHECK(out[1] == 500);
    CHECK(out[2] == 1000);
    CHECK(out[3] == 1000);
    return 0;
}

static int test_mono_to_stereo(void)
{
    static const int16_t in[] = {1000};
    int16_t out[2];
    BinkMixTrack track;

    memset(&track, 0, sizeof(track));
    track.pcm = in;
    track.samples = 1;
    track.rate = 1000;
    track.channels = 1;
    CHECK(bink_mix(&track, 1, 1000, 2, out, 1) == 0);
    CHECK(out[0] == 1000);
    CHECK(out[1] == 1000);
    return 0;
}

static int test_saturate(void)
{
    static const int16_t a[] = {20000};
    static const int16_t b[] = {20000};
    int16_t out[1];
    BinkMixTrack tracks[2];

    memset(tracks, 0, sizeof(tracks));
    tracks[0].pcm = a;
    tracks[0].samples = 1;
    tracks[0].rate = 1000;
    tracks[0].channels = 1;
    tracks[1].pcm = b;
    tracks[1].samples = 1;
    tracks[1].rate = 1000;
    tracks[1].channels = 1;
    CHECK(bink_mix(tracks, 2, 1000, 1, out, 1) == 0);
    CHECK(out[0] == 32767);
    return 0;
}

static int test_sum(void)
{
    static const int16_t a[] = {100};
    static const int16_t b[] = {50};
    int16_t out[1];
    BinkMixTrack tracks[2];

    memset(tracks, 0, sizeof(tracks));
    tracks[0].pcm = a;
    tracks[0].samples = 1;
    tracks[0].rate = 1000;
    tracks[0].channels = 1;
    tracks[1].pcm = b;
    tracks[1].samples = 1;
    tracks[1].rate = 1000;
    tracks[1].channels = 1;
    CHECK(bink_mix(tracks, 2, 1000, 1, out, 1) == 0);
    CHECK(out[0] == 150);
    return 0;
}

static int test_null_pcm(void)
{
    static const int16_t a[] = {1000};
    int16_t out[1];
    BinkMixTrack tracks[2];

    memset(tracks, 0, sizeof(tracks));
    out[0] = 7;
    tracks[0].pcm = a;
    tracks[0].samples = 1;
    tracks[0].rate = 1000;
    tracks[0].channels = 1;
    tracks[1].pcm = NULL;
    tracks[1].samples = 4;
    tracks[1].rate = 1000;
    tracks[1].channels = 1;
    CHECK(bink_mix(tracks, 2, 1000, 1, out, 1) == 0);
    CHECK(out[0] == 1000);
    return 0;
}

static int test_reject(void)
{
    static const int16_t in[] = {1000};
    int16_t out[1];
    BinkMixTrack track;

    memset(&track, 0, sizeof(track));
    track.pcm = in;
    track.samples = 1;
    track.rate = 1000;
    track.channels = 1;
    CHECK(bink_mix(&track, 1, 1000, 1, NULL, 1) == -1);
    CHECK(bink_mix(&track, 1, 0, 1, out, 1) == -1);
    return 0;
}

int main(void)
{
    if (test_upsample() != 0)
        return 1;
    if (test_mono_to_stereo() != 0)
        return 1;
    if (test_saturate() != 0)
        return 1;
    if (test_sum() != 0)
        return 1;
    if (test_null_pcm() != 0)
        return 1;
    if (test_reject() != 0)
        return 1;
    return 0;
}
