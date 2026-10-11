#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * The mixer: every stream is read at its own rate - a linear
 * interpolation between two of its frames (`prev`, `cur`) moved by `step`
 * source frames per device frame -, in the device's channels, scaled by
 * its volume and summed into Q24 accumulators.
 */

/* The next frame of a stream into `cur`, false when it has none */
static bool next_frame(struct libdmaudio_device* d, struct libdmaudio_stream* s)
{
    memcpy(s->prev, s->cur, d->format.channels * sizeof(int32_t));
    uint32_t tail = s->tail;
    if (__atomic_load_n(&s->head, __ATOMIC_ACQUIRE) == tail)
    {
        memset(s->cur, 0, d->format.channels * sizeof(int32_t));
        if (s->playing)
        {
            s->playing = false;
            s->underruns++;
        }
        return false;
    }
    read_frame(&s->format, s->ring + (tail % s->capacity) * s->frame_bytes, d->format.channels, s->cur);
    __atomic_store_n(&s->tail, tail + 1U, __ATOMIC_RELEASE);
    s->playing = true;
    s->played++;
    return true;
}

/* Adds `frames` frames of a stream to the accumulators; true when it had samples */
static bool mix_stream(struct libdmaudio_device* d, struct libdmaudio_stream* s, int32_t* acc, uint32_t frames)
{
    uint8_t channels = d->format.channels;
    int64_t gain = (int64_t)__atomic_load_n(&s->gain, __ATOMIC_RELAXED);
    bool had = false;
    bool same_rate = s->step == PHASE_ONE;

    for (uint32_t f = 0; f < frames; f++, acc += channels)
    {
        if (same_rate)
        {
            if (next_frame(d, s))
                had = true;
            for (uint8_t c = 0; c < channels; c++)
                acc[c] += (int32_t)((s->cur[c] * gain) >> 15);
            continue;
        }
        s->phase += s->step;
        while (s->phase >= PHASE_ONE)
        {
            s->phase -= PHASE_ONE;
            if (next_frame(d, s))
                had = true;
        }
        int64_t frac = s->phase;
        for (uint8_t c = 0; c < channels; c++)
        {
            int64_t v = s->prev[c] + (((int64_t)(s->cur[c] - s->prev[c]) * frac) >> 16);
            acc[c] += (int32_t)((v * gain) >> 15);
        }
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
