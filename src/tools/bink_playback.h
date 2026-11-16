#ifndef BINK_PLAYBACK_H
#define BINK_PLAYBACK_H

/* Audio output and seek glue shared by every player. The device layer is
 * provided per backend by bink_playback_sdl.c or bink_playback_win32.c. */

#include "bink.h"

typedef struct {
    int ready;
    int rate;
    int channels;
    void *device; /* backend context */
} BinkPlayback;

int bink_playback_audio_subsystem(void);
int bink_playback_audio_open(BinkPlayback *p, int rate, int channels);
void bink_playback_audio_close(BinkPlayback *p);
void bink_playback_audio_clear(BinkPlayback *p);
unsigned long bink_playback_queued(const BinkPlayback *p);
void bink_playback_queue(BinkPlayback *p, const void *data, unsigned long bytes);
void bink_playback_set_paused(BinkPlayback *p, int pause_on, int *device_paused);
/* Mix the current frame's enabled tracks and queue them. track_channels and
 * track_rates come from bink_info_audio. total_bytes may be NULL. */
void bink_playback_publish(bink object, const int enabled[7],
                           const unsigned char track_channels[7],
                           const unsigned long track_rates[7],
                           BinkPlayback *p, unsigned long *total_bytes);
/* Decode forward to target, rebuilding the audio queue from the seek origin. */
int bink_playback_seek(bink object, unsigned long target, char *status,
                       BinkPlayback *p, double usf, const int enabled[7],
                       const unsigned char track_channels[7],
                       const unsigned long track_rates[7]);
/* Release the mix buffer; call once at exit. */
void bink_playback_shutdown(void);

#endif
