#ifndef LIBDMAUDIO_PRIVATE_H
#define LIBDMAUDIO_PRIVATE_H

#include "dmod.h"
#include "dmosi.h"
#include "libdmaudio.h"

/*
 * Samples are mixed as Q24: signed 24-bit values in int32 (S16 << 8,
 * S24 as it is, S32 >> 8) - room for 128 full-scale streams before the
 * sum overflows, clamped to 24 bits when it is written out.
 */
#define Q24_MAX             0x007FFFFF
#define Q24_MIN             (-0x00800000)
#define GAIN_ONE            32768u          /* Q15 */
#define PHASE_ONE           0x10000u        /* Q16: a source frame */
#define MIX_CHUNK           64u             /* Frames mixed at once */

#define STREAM_MAGIC        0x41535452u     /* 'ASTR' */
#define DEVICE_MAGIC        0x41444556u     /* 'ADEV' */

struct libdmaudio_device;

struct libdmaudio_stream
{
    uint32_t                    magic;
    struct libdmaudio_stream*   next;
    struct libdmaudio_device*   device;         /* NULL: orphaned (under its lock) */
    dmosi_process_id_t          pid;
    libdmaudio_format_t         format;
    uint32_t                    frame_bytes;

    /* Ring: written by the application, read by the mixer - one each, no
     * lock; `head` and `tail` count frames and wrap */
    uint8_t*                    ring;
    uint32_t                    capacity;       /* Frames */
    uint32_t                    head;           /* Written (by the application) */
    uint32_t                    tail;           /* Mixed (by the mixer) */
    dmosi_semaphore_t           space;          /* Posted when the mixer took frames - */
    uint32_t                    waiters;        /* - and the application waits for it (1) */

    /* Mixer state */
    uint32_t                    gain;           /* Q15 */
    uint32_t                    step;           /* Source frames per device frame, Q16 */
    uint32_t                    phase;          /* Q16, between `prev` and `cur` */
    int32_t                     prev[LIBDMAUDIO_MAX_CHANNELS];   /* In device channels, Q24 */
    int32_t                     cur[LIBDMAUDIO_MAX_CHANNELS];
    bool                        playing;        /* Had samples - running dry is an underrun */
    bool                        paused;
    uint32_t                    underruns;
    uint64_t                    played;
};

struct libdmaudio_device
{
    uint32_t                    magic;
    struct libdmaudio_device*   next;
    char*                       name;
    libdmaudio_format_t         format;
    uint32_t                    frame_bytes;
    dmosi_mutex_t               lock;           /* Its streams, the mix */
    struct libdmaudio_stream*   streams;
    uint32_t                    count;
    dmosi_semaphore_t           wakeup;         /* A stream got samples */
    bool                        waiting;        /* The device waits for one (libdmaudio_device_wait()) */
    int32_t*                    acc;            /* MIX_CHUNK frames, Q24 */
    uint32_t                    reaped_at;      /* Tick of the last look for streams of processes that are gone */

    /* Master volume: asked by anybody, applied by the service */
    uint8_t                     volume;
    bool                        muted;
    uint32_t                    settings;       /* Changes with every request */
    uint32_t                    soft_gain;      /* Q15 - the node has no volume: applied to the mix */
    bool                        running;
};

/* registry.c */
extern dmosi_mutex_t g_lock;
struct libdmaudio_device* device_find(const char* name);   /* Under g_lock */

void stream_wake(struct libdmaudio_stream* s);
void streams_reap(struct libdmaudio_device* d);

/* convert.c */
uint32_t format_bytes(dmdrvi_audio_format_t format);
bool format_valid(const libdmaudio_format_t* format);
void read_frame(const libdmaudio_format_t* from, const uint8_t* src, uint8_t to_channels, int32_t* out);
void write_frames(const libdmaudio_format_t* to, const int32_t* in, uint32_t frames, uint8_t* dst);
uint32_t gain_of(uint8_t percent);

#endif /* LIBDMAUDIO_PRIVATE_H */
