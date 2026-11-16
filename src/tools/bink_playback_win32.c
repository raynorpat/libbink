/* waveOut backend for the shared playback module. */

#include "tools/bink_playback.h"

#include <windows.h>
#include <mmsystem.h>

#include <stdlib.h>
#include <string.h>

#define BINK_WAVE_BUFFERS 8

typedef struct {
    HWAVEOUT out;
    WAVEHDR header[BINK_WAVE_BUFFERS];
    unsigned char *data[BINK_WAVE_BUFFERS];
    size_t capacity[BINK_WAVE_BUFFERS];
    unsigned next;
} Win32Device;

static Win32Device *win32_of(BinkPlayback *p)
{
    return (Win32Device *)p->device;
}

int bink_playback_audio_subsystem(void)
{
    return 0;
}

int bink_playback_audio_open(BinkPlayback *p, int rate, int channels)
{
    WAVEFORMATEX format;
    Win32Device *dev;

    p->rate = rate;
    p->channels = channels;
    p->ready = 0;
    if (rate <= 0 || channels <= 0)
        return -1;
    dev = (Win32Device *)calloc(1, sizeof(*dev));
    if (dev == NULL)
        return -1;

    memset(&format, 0, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = (WORD)channels;
    format.nSamplesPerSec = (DWORD)rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = (WORD)(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    if (waveOutOpen(&dev->out, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) !=
        MMSYSERR_NOERROR) {
        free(dev);
        return -1;
    }
    {
        unsigned i;

        for (i = 0; i < BINK_WAVE_BUFFERS; i++) {
            memset(&dev->header[i], 0, sizeof(dev->header[i]));
            dev->header[i].dwUser = i;
        }
    }
    p->device = dev;
    p->ready = 1;
    return 0;
}

/* Releases headers the device finished with, so their buffers can be
 * filled again. */
static void reclaim(Win32Device *dev)
{
    unsigned i;

    for (i = 0; i < BINK_WAVE_BUFFERS; i++) {
        WAVEHDR *header = &dev->header[i];

        if (header->dwFlags & WHDR_PREPARED) {
            if (header->dwFlags & WHDR_DONE) {
                waveOutUnprepareHeader(dev->out, header, sizeof(*header));
                memset(header, 0, sizeof(*header));
                header->dwUser = i;
            }
        }
    }
}

void bink_playback_audio_close(BinkPlayback *p)
{
    Win32Device *dev = win32_of(p);
    unsigned i;

    if (!p->ready || dev == NULL)
        return;
    waveOutReset(dev->out);
    for (i = 0; i < BINK_WAVE_BUFFERS; i++) {
        if (dev->header[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(dev->out, &dev->header[i],
                                   sizeof(dev->header[i]));
        free(dev->data[i]);
    }
    waveOutClose(dev->out);
    free(dev);
    p->device = NULL;
    p->ready = 0;
}

void bink_playback_audio_clear(BinkPlayback *p)
{
    Win32Device *dev = win32_of(p);

    if (!p->ready || dev == NULL)
        return;
    waveOutReset(dev->out);
    reclaim(dev);
}

unsigned long bink_playback_queued(const BinkPlayback *p)
{
    Win32Device *dev = (Win32Device *)p->device;
    unsigned long total = 0;
    unsigned i;

    if (!p->ready || dev == NULL)
        return 0;
    for (i = 0; i < BINK_WAVE_BUFFERS; i++) {
        const WAVEHDR *header = &dev->header[i];

        if ((header->dwFlags & WHDR_PREPARED) &&
            !(header->dwFlags & WHDR_DONE))
            total += (unsigned long)header->dwBufferLength;
    }
    return total;
}

void bink_playback_queue(BinkPlayback *p, const void *data, unsigned long bytes)
{
    Win32Device *dev = win32_of(p);
    WAVEHDR *header;
    unsigned slot;
    int tries;

    if (!p->ready || dev == NULL || data == NULL || bytes == 0)
        return;
    /* Wait briefly for a free buffer rather than drop audio. */
    for (tries = 0; tries < 100; tries++) {
        reclaim(dev);
        slot = dev->next % BINK_WAVE_BUFFERS;
        if (!(dev->header[slot].dwFlags & WHDR_PREPARED))
            goto found;
        dev->next++;
        Sleep(1);
    }
    return;

found:
    header = &dev->header[slot];
    if (dev->capacity[slot] < bytes) {
        unsigned char *grown =
            (unsigned char *)realloc(dev->data[slot], (size_t)bytes);

        if (grown == NULL)
            return;
        dev->data[slot] = grown;
        dev->capacity[slot] = (size_t)bytes;
    }
    memcpy(dev->data[slot], data, (size_t)bytes);
    header->lpData = (LPSTR)dev->data[slot];
    header->dwBufferLength = (DWORD)bytes;
    header->dwFlags = 0;
    if (waveOutPrepareHeader(dev->out, header, sizeof(*header)) !=
        MMSYSERR_NOERROR) {
        memset(header, 0, sizeof(*header));
        header->dwUser = slot;
        return;
    }
    if (waveOutWrite(dev->out, header, sizeof(*header)) != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(dev->out, header, sizeof(*header));
        memset(header, 0, sizeof(*header));
        header->dwUser = slot;
        return;
    }
    dev->next = slot + 1;
}

void bink_playback_set_paused(BinkPlayback *p, int pause_on, int *device_paused)
{
    Win32Device *dev = win32_of(p);

    if (!p->ready || dev == NULL || pause_on == *device_paused)
        return;
    if (pause_on)
        waveOutPause(dev->out);
    else
        waveOutRestart(dev->out);
    *device_paused = pause_on;
}
