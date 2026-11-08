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

static int test_short_track_ends(void)
{
    static const int16_t longer[] = {10, 20, 30, 40};
    static const int16_t shorter[] = {1, 3};
    int16_t out[4];
    BinkMixTrack tracks[2];

    memset(tracks, 0, sizeof(tracks));
    tracks[0].pcm = longer;
    tracks[0].samples = 4;
    tracks[0].rate = 1000;
    tracks[0].channels = 1;
    tracks[1].pcm = shorter;
    tracks[1].samples = 2;
    tracks[1].rate = 1000;
    tracks[1].channels = 1;
    CHECK(bink_mix(tracks, 2, 1000, 1, out, 4) == 0);
    CHECK(out[0] == 11);
    CHECK(out[1] == 23);
    CHECK(out[2] == 30);
    CHECK(out[3] == 40);
    return 0;
}

static int test_resample_past_end(void)
{
    static const int16_t in[] = {100};
    int16_t out[4];
    BinkMixTrack track;

    memset(&track, 0, sizeof(track));
    track.pcm = in;
    track.samples = 1;
    track.rate = 1000;
    track.channels = 1;
    CHECK(bink_mix(&track, 1, 2000, 1, out, 4) == 0);
    CHECK(out[0] == 100);
    CHECK(out[1] == 100);
    CHECK(out[2] == 0);
    CHECK(out[3] == 0);
    return 0;
}

static int test_audio_clock(void)
{
    /* logo_lucas.bik frame 0 is 134400 bytes, about 0.76 s. One picture
       is 5867 bytes. The picture advances after that much has played,
       while the rest of the burst stays queued. */
    CHECK(bink_frame_bytes(44000, 2, 1000000.0 / 30.0) == 5867);
    CHECK(bink_audio_waiting(134400, 134400, 5867) == 1);
    CHECK(bink_audio_waiting(134400, 134400 - 5866, 5867) == 1);
    CHECK(bink_audio_waiting(134400, 134400 - 5867, 5867) == 0);
    CHECK(bink_audio_waiting(134400, 11733, 5867) == 0);
    CHECK(bink_audio_waiting(134400, 0, 5867) == 0);
    CHECK(bink_soundtrack_offset(44000, 2, 1000000.0 / 30.0, 30) == 176000UL);
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
    if (test_short_track_ends() != 0)
        return 1;
    if (test_resample_past_end() != 0)
        return 1;
    if (test_audio_clock() != 0)
        return 1;
    if (test_reject() != 0)
        return 1;
    return 0;
}
