#define DMOD_ENABLE_REGISTRATION ON
#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * Devices and their streams - the state the library shares between the
 * services (one per device) and the applications.
 *
 * A stream remembers the pid of the process that opened it: when the
 * process is gone without closing it, the mixer drops it (every second).
 * Everything is allocated by the library, so it outlives the processes.
 */

#define REAP_PERIOD_MS  1000u

dmosi_mutex_t                       g_lock;     /* The list of devices */
static struct libdmaudio_device*    g_devices;  /* Newest first */

int dmod_init(const Dmod_Config_t* Config)
{
    (void)Config;
    g_lock = dmosi_mutex_create(false);
    return (g_lock != NULL) ? 0 : -ENOMEM;
}

int dmod_deinit(void)
{
    while (g_devices != NULL)
        libdmaudio_device_unregister(g_devices);
    if (g_lock != NULL)
        dmosi_mutex_destroy(g_lock);
    g_lock = NULL;
    return 0;
}

static bool is_device(libdmaudio_device_t d)
{
    return d != NULL && d->magic == DEVICE_MAGIC;
}

static bool is_stream(libdmaudio_stream_t s)
{
    return s != NULL && s->magic == STREAM_MAGIC;
}

static dmosi_process_id_t current_pid(void)
{
    dmosi_process_t process = dmosi_process_current();
    return (process != NULL) ? dmosi_process_get_id(process) : 0;
}

/* NULL: the first one registered - the last in the list */
struct libdmaudio_device* device_find(const char* name)
{
    struct libdmaudio_device* found = NULL;
    for (struct libdmaudio_device* d = g_devices; d != NULL; d = d->next)
    {
        if (name == NULL)
            found = d;
        else if (strcmp(d->name, name) == 0)
            return d;
    }
    return found;
}

/* ---- Devices ---- */

dmod_libdmaudio_api_declaration(1.0, libdmaudio_device_t, _device_register, ( const char* name, const libdmaudio_format_t* format ))
{
    if (name == NULL || !format_valid(format) || g_lock == NULL)
        return NULL;
    struct libdmaudio_device* d = Dmod_Malloc(sizeof(*d));
    if (d == NULL)
        return NULL;
    memset(d, 0, sizeof(*d));
    d->magic = DEVICE_MAGIC;
    d->format = *format;
    d->frame_bytes = format_bytes(format->format) * format->channels;
    d->name = Dmod_StrDup(name);
    d->lock = dmosi_mutex_create(false);
    d->wakeup = dmosi_semaphore_create(0, 4);       /* (a post a timed-out wait left - see libdmaudio_device_wait()) */
    d->acc = Dmod_Malloc(MIX_CHUNK * format->channels * sizeof(int32_t));
    d->volume = 100;
    d->soft_gain = GAIN_ONE;
    d->reaped_at = dmosi_get_tick_count();
    if (d->name == NULL || d->lock == NULL || d->wakeup == NULL || d->acc == NULL)
        goto fail;

    dmosi_mutex_lock(g_lock);
    for (struct libdmaudio_device* o = g_devices; o != NULL; o = o->next)
    {
        if (strcmp(o->name, name) == 0)
        {
            dmosi_mutex_unlock(g_lock);
            goto fail;
        }
    }
    d->next = g_devices;
    g_devices = d;
    dmosi_mutex_unlock(g_lock);
    return d;

fail:
    if (d->name != NULL)
        Dmod_Free(d->name);
    if (d->lock != NULL)
        dmosi_mutex_destroy(d->lock);
    if (d->wakeup != NULL)
        dmosi_semaphore_destroy(d->wakeup);
    if (d->acc != NULL)
        Dmod_Free(d->acc);
    Dmod_Free(d);
    return NULL;
}

dmod_libdmaudio_api_declaration(1.0, void, _device_unregister, ( libdmaudio_device_t device ))
{
    if (!is_device(device) || g_lock == NULL)
        return;
    dmosi_mutex_lock(g_lock);
    for (struct libdmaudio_device** p = &g_devices; *p != NULL; p = &(*p)->next)
    {
        if (*p == device)
        {
            *p = device->next;
            break;
        }
    }
    /* Its streams stay with their applications until they close them */
    dmosi_mutex_lock(device->lock);
    for (struct libdmaudio_stream* s = device->streams; s != NULL; s = s->next)
    {
        s->device = NULL;
        stream_wake(s);                                 /* A writer waiting: -ENODEV */
    }
    device->streams = NULL;
    dmosi_mutex_unlock(device->lock);
    dmosi_mutex_unlock(g_lock);

    device->magic = 0;
    dmosi_mutex_destroy(device->lock);
    dmosi_semaphore_destroy(device->wakeup);
    Dmod_Free(device->acc);
    Dmod_Free(device->name);
    Dmod_Free(device);
}

dmod_libdmaudio_api_declaration(1.0, int, _device_info, ( const char* device, libdmaudio_device_info_t* info ))
{
    if (info == NULL || g_lock == NULL)
        return -EINVAL;
    dmosi_mutex_lock(g_lock);
    struct libdmaudio_device* d = device_find(device);
    if (d != NULL)
    {
        memset(info, 0, sizeof(*info));
        strncpy(info->name, d->name, sizeof(info->name) - 1U);
        info->format = d->format;
        info->streams = d->count;
        info->volume_percent = d->volume;
        info->muted = d->muted;
        info->running = d->running;
    }
    dmosi_mutex_unlock(g_lock);
    return (d != NULL) ? 0 : -ENODEV;
}

dmod_libdmaudio_api_declaration(1.0, int, _device_name, ( uint32_t index, char* name, size_t size ))
{
    if (name == NULL || g_lock == NULL)
        return -EINVAL;
    dmosi_mutex_lock(g_lock);
    uint32_t count = 0;
    for (struct libdmaudio_device* d = g_devices; d != NULL; d = d->next)
        count++;
    /* Index 0 is the first registered: the last in the list */
    struct libdmaudio_device* d = g_devices;
    for (uint32_t i = 0; d != NULL && i + 1U + index < count; i++)
        d = d->next;
    int ret = -ENOENT;
    if (index < count && d != NULL)
    {
        size_t len = strlen(d->name);
        ret = -ERANGE;
        if (size > len)
        {
            memcpy(name, d->name, len + 1U);
            ret = 0;
        }
    }
    dmosi_mutex_unlock(g_lock);
    return ret;
}

static int set_master(const char* device, int volume, int muted)
{
    if (g_lock == NULL)
        return -EINVAL;
    dmosi_mutex_lock(g_lock);
    struct libdmaudio_device* d = device_find(device);
    if (d != NULL)
    {
        if (volume >= 0)
            d->volume = (uint8_t)((volume > 100) ? 100 : volume);
        if (muted >= 0)
            d->muted = muted != 0;
        d->settings++;
    }
    dmosi_mutex_unlock(g_lock);
    return (d != NULL) ? 0 : -ENODEV;
}

dmod_libdmaudio_api_declaration(1.0, int, _device_set_volume, ( const char* device, uint8_t volume_percent ))
{
    return set_master(device, volume_percent, -1);
}

dmod_libdmaudio_api_declaration(1.0, int, _device_set_mute, ( const char* device, bool muted ))
{
    return set_master(device, -1, muted ? 1 : 0);
}

dmod_libdmaudio_api_declaration(1.0, int, _device_wait, ( libdmaudio_device_t device, int32_t timeout_ms ))
{
    if (!is_device(device))
        return -EINVAL;
    for (;;)
    {
        bool ready = false;
        dmosi_mutex_lock(device->lock);
        for (struct libdmaudio_stream* s = device->streams; s != NULL && !ready; s = s->next)
            ready = !s->paused && __atomic_load_n(&s->head, __ATOMIC_ACQUIRE) != s->tail;
        device->waiting = !ready;
        dmosi_mutex_unlock(device->lock);
        if (ready)
            return 0;
        if (dmosi_semaphore_wait(device->wakeup, 1, timeout_ms) != 0)
        {
            device->waiting = false;
            return -ETIMEDOUT;
        }
    }
}

/* ---- Streams ---- */

/*
 * The application waits for the mixer on `space`; the mixer posts it only
 * when `waiters` says somebody waits (posting a full semaphore is an error),
 * so it never holds more than the one post the waiter takes.
 */
void stream_wake(struct libdmaudio_stream* s)
{
    if (__atomic_exchange_n(&s->waiters, 0u, __ATOMIC_SEQ_CST) != 0u)
        (void)dmosi_semaphore_post(s->space, 1);
}

/* Wait until the mixer takes frames past `tail` (true) or `left` ms pass */
static bool wait_mixer(struct libdmaudio_stream* s, uint32_t tail, int32_t left)
{
    __atomic_store_n(&s->waiters, 1u, __ATOMIC_SEQ_CST);
    bool moved = __atomic_load_n(&s->tail, __ATOMIC_SEQ_CST) != tail || s->device == NULL;
    if (!moved && dmosi_semaphore_wait(s->space, 1, left) == 0)
        return true;
    /* Not waiting any more - a post made meanwhile is taken back */
    if (__atomic_exchange_n(&s->waiters, 0u, __ATOMIC_SEQ_CST) == 0u)
    {
        (void)dmosi_semaphore_wait(s->space, 1, -1);
        return true;
    }
    return moved;
}

static void wake_device(libdmaudio_stream_t s)
{
    /* Rare - only while the device idles - so the lock costs nothing */
    dmosi_mutex_lock(g_lock);
    struct libdmaudio_device* d = s->device;
    if (d != NULL && d->waiting)
    {
        d->waiting = false;
        (void)dmosi_semaphore_post(d->wakeup, 1);
    }
    dmosi_mutex_unlock(g_lock);
}

dmod_libdmaudio_api_declaration(1.0, libdmaudio_stream_t, _open, ( const char* device, const libdmaudio_format_t* format, const libdmaudio_stream_options_t* options, int* status ))
{
    int dummy;
    if (status == NULL)
        status = &dummy;
    *status = -EINVAL;
    if (!format_valid(format) || g_lock == NULL)
        return NULL;

    uint32_t buffer_ms = (options != NULL && options->buffer_ms != 0) ? options->buffer_ms : LIBDMAUDIO_DEFAULT_BUFFER_MS;
    uint64_t frames = (uint64_t)format->sample_rate_hz * buffer_ms / 1000u;
    if (frames < 64u)
        frames = 64u;
    if (frames > 0x00FFFFFFu)
        return NULL;

    struct libdmaudio_stream* s = Dmod_Malloc(sizeof(*s));
    if (s == NULL)
    {
        *status = -ENOMEM;
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->magic = STREAM_MAGIC;
    s->pid = current_pid();
    s->format = *format;
    s->frame_bytes = format_bytes(format->format) * format->channels;
    s->capacity = (uint32_t)frames;
    s->ring = Dmod_Malloc(s->capacity * s->frame_bytes);
    s->space = dmosi_semaphore_create(0, 1);
    s->gain = gain_of((options != NULL) ? options->volume_percent : 100u);
    if (s->ring == NULL || s->space == NULL)
    {
        if (s->ring != NULL)
            Dmod_Free(s->ring);
        if (s->space != NULL)
            dmosi_semaphore_destroy(s->space);
        Dmod_Free(s);
        *status = -ENOMEM;
        return NULL;
    }

    dmosi_mutex_lock(g_lock);
    struct libdmaudio_device* d = device_find(device);
    if (d != NULL)
    {
        s->step = (uint32_t)(((uint64_t)format->sample_rate_hz * PHASE_ONE + d->format.sample_rate_hz / 2u) / d->format.sample_rate_hz);
        dmosi_mutex_lock(d->lock);
        s->device = d;
        s->next = d->streams;
        d->streams = s;
        d->count++;
        dmosi_mutex_unlock(d->lock);
    }
    dmosi_mutex_unlock(g_lock);

    if (d == NULL)
    {
        Dmod_Free(s->ring);
        dmosi_semaphore_destroy(s->space);
        Dmod_Free(s);
        *status = -ENODEV;
        return NULL;
    }
    *status = 0;
    return s;
}

static void stream_free(struct libdmaudio_stream* s)
{
    s->magic = 0;
    Dmod_Free(s->ring);
    dmosi_semaphore_destroy(s->space);
    Dmod_Free(s);
}

/* Unlink a stream from its device; under g_lock */
static void stream_unlink(struct libdmaudio_stream* s)
{
    struct libdmaudio_device* d = s->device;
    if (d == NULL)
        return;
    dmosi_mutex_lock(d->lock);
    for (struct libdmaudio_stream** p = &d->streams; *p != NULL; p = &(*p)->next)
    {
        if (*p == s)
        {
            *p = s->next;
            d->count--;
            break;
        }
    }
    s->device = NULL;
    dmosi_mutex_unlock(d->lock);
}

dmod_libdmaudio_api_declaration(1.0, void, _close, ( libdmaudio_stream_t stream ))
{
    if (!is_stream(stream) || g_lock == NULL)
        return;
    dmosi_mutex_lock(g_lock);
    stream_unlink(stream);
    dmosi_mutex_unlock(g_lock);
    stream_free(stream);
}

/* Streams of processes that are gone; the device's lock is held */
void streams_reap(struct libdmaudio_device* d)
{
    uint32_t now = dmosi_get_tick_count();
    if (now - d->reaped_at < REAP_PERIOD_MS)
        return;
    d->reaped_at = now;
    for (struct libdmaudio_stream** p = &d->streams; *p != NULL; )
    {
        struct libdmaudio_stream* s = *p;
        if (s->pid != 0 && dmosi_process_find_by_id(s->pid) == NULL)
        {
            *p = s->next;
            d->count--;
            stream_free(s);
        }
        else
            p = &s->next;
    }
}

dmod_libdmaudio_api_declaration(1.0, int32_t, _write, ( libdmaudio_stream_t stream, const void* data, uint32_t frames, int32_t timeout_ms ))
{
    if (!is_stream(stream) || (data == NULL && frames != 0))
        return -EINVAL;
    const uint8_t* src = data;
    uint32_t written = 0;
    uint32_t start = dmosi_get_tick_count();
    bool was_empty = false;

    while (written < frames)
    {
        if (stream->device == NULL)
            return (written > 0) ? (int32_t)written : -ENODEV;
        uint32_t head = stream->head;
        uint32_t tail = __atomic_load_n(&stream->tail, __ATOMIC_ACQUIRE);
        uint32_t room = stream->capacity - (head - tail);
        if (room == 0)
        {
            int32_t left = timeout_ms;
            if (timeout_ms > 0)
            {
                uint32_t spent = dmosi_get_tick_count() - start;
                left = (spent >= (uint32_t)timeout_ms) ? 0 : timeout_ms - (int32_t)spent;
            }
            if (left == 0 || !wait_mixer(stream, tail, left))
                break;
            continue;
        }
        if (head == tail)
            was_empty = true;
        uint32_t n = frames - written;
        if (n > room)
            n = room;
        uint32_t at = head % stream->capacity;
        uint32_t first = stream->capacity - at;
        if (first > n)
            first = n;
        memcpy(stream->ring + at * stream->frame_bytes, src, first * stream->frame_bytes);
        if (n > first)
            memcpy(stream->ring, src + first * stream->frame_bytes, (n - first) * stream->frame_bytes);
        __atomic_store_n(&stream->head, head + n, __ATOMIC_RELEASE);
        src += n * stream->frame_bytes;
        written += n;
    }
    if (was_empty && written > 0)
        wake_device(stream);
    return (written > 0 || frames == 0) ? (int32_t)written : -ETIMEDOUT;
}

dmod_libdmaudio_api_declaration(1.0, int, _drain, ( libdmaudio_stream_t stream, int32_t timeout_ms ))
{
    if (!is_stream(stream))
        return -EINVAL;
    uint32_t start = dmosi_get_tick_count();
    for (;;)
    {
        if (stream->device == NULL)
            return -ENODEV;
        uint32_t tail = __atomic_load_n(&stream->tail, __ATOMIC_ACQUIRE);
        if (tail == stream->head)
            return 0;
        int32_t left = -1;
        if (timeout_ms >= 0)
        {
            uint32_t spent = dmosi_get_tick_count() - start;
            if (spent >= (uint32_t)timeout_ms)
                return -ETIMEDOUT;
            left = timeout_ms - (int32_t)spent;
        }
        (void)wait_mixer(stream, tail, left);
    }
}

dmod_libdmaudio_api_declaration(1.0, int, _set_volume, ( libdmaudio_stream_t stream, uint8_t volume_percent ))
{
    if (!is_stream(stream))
        return -EINVAL;
    __atomic_store_n(&stream->gain, gain_of(volume_percent), __ATOMIC_RELAXED);
    return 0;
}

dmod_libdmaudio_api_declaration(1.0, int, _set_paused, ( libdmaudio_stream_t stream, bool paused ))
{
    if (!is_stream(stream))
        return -EINVAL;
    stream->paused = paused;
    if (!paused && stream->head != stream->tail)
        wake_device(stream);
    return 0;
}

dmod_libdmaudio_api_declaration(1.0, int, _get_status, ( libdmaudio_stream_t stream, libdmaudio_stream_status_t* status ))
{
    if (!is_stream(stream) || status == NULL)
        return -EINVAL;
    memset(status, 0, sizeof(*status));
    status->buffered_frames = stream->head - __atomic_load_n(&stream->tail, __ATOMIC_ACQUIRE);
    status->buffer_frames = stream->capacity;
    status->played_frames = stream->played;
    status->underruns = stream->underruns;
    status->paused = stream->paused;
    status->orphaned = stream->device == NULL;
    return 0;
}
