#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * The mixer: every stream is read at its own rate - a linear
 * interpolation between two of its frames (`prev`, `cur`) moved by `step`
 * source frames per device frame -, in the device's channels, scaled by
 * its volume and summed into Q24 accumulators.
 *
 * It runs for every frame the device plays, so a stream's ring is read
 * without a call or an atomic per frame: its head is looked at once per
 * chunk, its tail given back once, and the usual stream - 16-bit samples
 * in the device's channels and rate - is summed straight from the ring.
 */

/* A stream's ring as the mixer reads it in a chunk */
typedef struct
{
    const uint8_t*  ring;
    uint32_t        frame_bytes;
    uint32_t        capacity;
    uint32_t        index;          /* tail % capacity */
    uint32_t        left;           /* Frames written and not mixed yet */
    uint32_t        taken;
} reader_t;

static inline void reader_open(reader_t* r, const struct libdmaudio_stream* s)
{
    r->ring = s->ring;
    r->frame_bytes = s->frame_bytes;
    r->capacity = s->capacity;
    r->index = s->tail % s->capacity;
    r->left = __atomic_load_n(&s->head, __ATOMIC_ACQUIRE) - s->tail;
    r->taken = 0;
}

static inline const uint8_t* reader_take(reader_t* r)
{
    const uint8_t* p = r->ring + r->index * r->frame_bytes;
    if (++r->index == r->capacity)
        r->index = 0;
    r->left--;
    r->taken++;
    return p;
}

/* A stream ran dry: silence from here, an underrun if it was playing */
static void run_dry(uint8_t channels, struct libdmaudio_stream* s)
{
    for (uint8_t c = 0; c < channels; c++)
        s->cur[c] = 0;
    if (s->playing)
    {
        s->playing = false;
        s->underruns++;
    }
}

/* The next frame of a stream into `cur`, false when it has none */
static bool next_frame(uint8_t channels, struct libdmaudio_stream* s, reader_t* r)
{
    for (uint8_t c = 0; c < channels; c++)
        s->prev[c] = s->cur[c];
    if (r->left == 0)
    {
        run_dry(channels, s);
        return false;
    }
    const uint8_t* p = reader_take(r);
    if (s->format.format == DMDRVI_AUDIO_FORMAT_S16_LE && s->format.channels == channels)
    {
        const int16_t* q = (const int16_t*)p;      /* (the ring and its frames are 2-byte aligned) */
        for (uint8_t c = 0; c < channels; c++)
            s->cur[c] = (int32_t)q[c] * 256;
    }
    else
    {
        read_frame(&s->format, p, channels, s->cur);
    }
    s->playing = true;
    return true;
}

/* The device's rate, 16-bit samples in its channels: summed straight from the ring */
static bool mix_direct(uint8_t channels, struct libdmaudio_stream* s, reader_t* r, int32_t* acc, uint32_t frames)
{
    /* (q * 256 * gain) >> 15 == (q * gain) >> 7: a Q24 sample scaled by a Q15
     * gain (at most GAIN_ONE), in 32 bits */
    int32_t gain = (int32_t)__atomic_load_n(&s->gain, __ATOMIC_RELAXED);
    uint32_t n = (r->left < frames) ? r->left : frames;
    const int16_t* q = NULL;

    if (channels == 2u)
    {
        for (uint32_t f = 0; f < n; f++, acc += 2)
        {
            q = (const int16_t*)reader_take(r);
            acc[0] += ((int32_t)q[0] * gain) >> 7;
            acc[1] += ((int32_t)q[1] * gain) >> 7;
        }
    }
    else
    {
        for (uint32_t f = 0; f < n; f++, acc += channels)
        {
            q = (const int16_t*)reader_take(r);
            for (uint8_t c = 0; c < channels; c++)
                acc[c] += ((int32_t)q[c] * gain) >> 7;
        }
    }
    if (n > 0)
    {
        for (uint8_t c = 0; c < channels; c++)
            s->cur[c] = (int32_t)q[c] * 256;
        s->playing = true;
    }
    if (n < frames)
        run_dry(channels, s);
    for (uint8_t c = 0; c < channels; c++)
        s->prev[c] = s->cur[c];
    return n > 0;
}

/* Adds `frames` frames of a stream to the accumulators; true when it had samples */
static bool mix_stream(struct libdmaudio_device* d, struct libdmaudio_stream* s, int32_t* acc, uint32_t frames)
{
    uint8_t channels = d->format.channels;
    bool same_rate = s->step == PHASE_ONE;
    bool had = false;
    reader_t r;
    reader_open(&r, s);

    if (same_rate && s->format.format == DMDRVI_AUDIO_FORMAT_S16_LE && s->format.channels == channels)
    {
        had = mix_direct(channels, s, &r, acc, frames);
    }
    else
    {
        int64_t gain = (int64_t)__atomic_load_n(&s->gain, __ATOMIC_RELAXED);
        for (uint32_t f = 0; f < frames; f++, acc += channels)
        {
            if (same_rate)
            {
                if (next_frame(channels, s, &r))
                    had = true;
                for (uint8_t c = 0; c < channels; c++)
                    acc[c] += (int32_t)((s->cur[c] * gain) >> 15);
                continue;
            }
            s->phase += s->step;
            while (s->phase >= PHASE_ONE)
            {
                s->phase -= PHASE_ONE;
                if (next_frame(channels, s, &r))
                    had = true;
            }
            int64_t frac = s->phase;
            for (uint8_t c = 0; c < channels; c++)
            {
                int64_t v = s->prev[c] + (((int64_t)(s->cur[c] - s->prev[c]) * frac) >> 16);
                acc[c] += (int32_t)((v * gain) >> 15);
            }
        }
    }

    if (r.taken > 0)
    {
        __atomic_store_n(&s->tail, s->tail + r.taken, __ATOMIC_RELEASE);
        s->played += r.taken;
    }
    return had;
}

dmod_libdmaudio_api_declaration(1.0, int, _device_mix, ( libdmaudio_device_t device, void* out, uint32_t frames ))
{
    if (device == NULL || device->magic != DEVICE_MAGIC || (out == NULL && frames != 0))
        return -EINVAL;
    uint8_t* dst = out;
    uint8_t channels = device->format.channels;
    int active = 0;

    dmosi_mutex_lock(device->lock);
    streams_reap(device);
    int64_t master = __atomic_load_n(&device->soft_gain, __ATOMIC_RELAXED);
    while (frames > 0)
    {
        uint32_t n = (frames > MIX_CHUNK) ? MIX_CHUNK : frames;
        memset(device->acc, 0, n * channels * sizeof(int32_t));
        int chunk_active = 0;
        for (struct libdmaudio_stream* s = device->streams; s != NULL; s = s->next)
        {
            if (s->paused)
                continue;
            uint32_t tail = s->tail;
            if (mix_stream(device, s, device->acc, n))
                chunk_active++;
            if (s->tail != tail)
                stream_wake(s);
        }
        if (chunk_active > active)
            active = chunk_active;
        if (master != GAIN_ONE)
        {
            for (uint32_t i = 0; i < n * channels; i++)
                device->acc[i] = (int32_t)((device->acc[i] * master) >> 15);
        }
        write_frames(&device->format, device->acc, n, dst);
        dst += n * device->frame_bytes;
        frames -= n;
    }
    dmosi_mutex_unlock(device->lock);
    return active;
}
