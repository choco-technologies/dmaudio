#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * Serving a device: its node (a codec, a bare serial audio interface - the
 * dmdrvi audio stream commands) is written a mixed period at a time; the
 * write waits for room in the node's DMA buffers, so the node's clock paces
 * the loop and the streams' buffers take up the scheduling of the
 * applications.
 */

#define IDLE_POLL_MS    100u    /* Stop requests are seen this fast while idle */

typedef struct
{
    void*               node;
    libdmaudio_device_t device;
    bool                running;
    bool                hw_volume;      /* The node has AUDIO_SET_VOLUME */
    bool                hw_mute;        /* ... and AUDIO_SET_MUTE */
    uint32_t            settings;       /* Of the device, applied */
} serve_t;

dmod_libdmaudio_api_declaration(1.0, void, _serve_defaults, ( libdmaudio_serve_options_t* options ))
{
    if (options == NULL)
        return;
    memset(options, 0, sizeof(*options));
    options->sample_rate_hz = 48000;
    options->channels = 2;
    options->sample_bits = 16;
    options->output = DMDRVI_AUDIO_OUTPUT_HEADPHONE;
    options->volume_percent = 70;
    options->period_ms = 10;
    options->idle_ms = 2000;
    options->timeout_ms = 1000;
}

static bool parse_number(const char* s, uint32_t min, uint32_t max, uint32_t* out)
{
    uint32_t v = 0;
    if (s == NULL || *s == '\0')
        return false;
    for (; *s != '\0'; s++)
    {
        if (*s < '0' || *s > '9' || v > 100000000u)
            return false;
        v = v * 10u + (uint32_t)(*s - '0');
    }
    if (v < min || v > max)
        return false;
    *out = v;
    return true;
}

dmod_libdmaudio_api_declaration(1.0, int, _serve_parse, ( int argc, char* argv[], libdmaudio_serve_options_t* options ))
{
    if (options == NULL || (argc > 0 && argv == NULL))
        return -EINVAL;
    for (int i = 0; i < argc; i++)
    {
        const char* opt = argv[i];
        const char* val = (i + 1 < argc) ? argv[i + 1] : NULL;
        uint32_t v = 0;
        bool ok = val != NULL;
        if (ok && strcmp(opt, "-r") == 0 && (ok = parse_number(val, 1000, 384000, &v)))
            options->sample_rate_hz = v;
        else if (ok && strcmp(opt, "-c") == 0 && (ok = parse_number(val, 1, LIBDMAUDIO_MAX_CHANNELS, &v)))
            options->channels = (uint8_t)v;
        else if (ok && strcmp(opt, "-b") == 0 && (ok = parse_number(val, 16, 32, &v)) && (v == 16 || v == 24 || v == 32))
            options->sample_bits = (uint8_t)v;
        else if (ok && strcmp(opt, "-v") == 0 && (ok = parse_number(val, 0, 100, &v)))
            options->volume_percent = (uint8_t)v;
        else if (ok && strcmp(opt, "-p") == 0 && (ok = parse_number(val, 1, 100, &v)))
            options->period_ms = v;
        else if (ok && strcmp(opt, "-i") == 0 && (ok = parse_number(val, 0, 3600000, &v)))
            options->idle_ms = v;
        else if (ok && strcmp(opt, "-t") == 0 && (ok = parse_number(val, 10, 60000, &v)))
            options->timeout_ms = v;
        else if (ok && strcmp(opt, "-o") == 0)
        {
            if (strcmp(val, "headphone") == 0)
                options->output = DMDRVI_AUDIO_OUTPUT_HEADPHONE;
            else if (strcmp(val, "speaker") == 0)
                options->output = DMDRVI_AUDIO_OUTPUT_SPEAKER;
            else if (strcmp(val, "line") == 0)
                options->output = DMDRVI_AUDIO_OUTPUT_LINE;
            else
                ok = false;
        }
        else
            ok = false;
        if (!ok)
        {
            DMOD_LOG_ERROR("libdmaudio: bad option %s%s%s\n", opt, (val != NULL) ? " " : "", (val != NULL) ? val : "");
            return -EINVAL;
        }
        i++;
    }
    return 0;
}

/* The master volume and mute of the device on the node - or on the mix, what the node cannot */
static void apply_settings(serve_t* s)
{
    struct libdmaudio_device* d = s->device;
    dmosi_mutex_lock(g_lock);
    uint8_t volume = d->volume;
    bool muted = d->muted;
    s->settings = d->settings;
    dmosi_mutex_unlock(g_lock);

    if (s->hw_volume && Dmod_Ioctl(s->node, DMDRVI_IOCTL_AUDIO_SET_VOLUME, &volume) == -ENOTSUP)
        s->hw_volume = false;
    if (s->hw_mute && Dmod_Ioctl(s->node, DMDRVI_IOCTL_AUDIO_SET_MUTE, &muted) == -ENOTSUP)
        s->hw_mute = false;
    uint32_t gain = s->hw_volume ? GAIN_ONE : gain_of(volume);
    if (muted && !s->hw_mute)
        gain = 0;
    __atomic_store_n(&d->soft_gain, gain, __ATOMIC_RELAXED);
}

static void set_running(serve_t* s, bool running)
{
    dmosi_mutex_lock(g_lock);
    s->device->running = running;
    dmosi_mutex_unlock(g_lock);
    s->running = running;
}

static void stop_node(serve_t* s)
{
    if (!s->running)
        return;
    (void)Dmod_Ioctl(s->node, DMDRVI_IOCTL_AUDIO_DRAIN, NULL);
    (void)Dmod_Ioctl(s->node, DMDRVI_IOCTL_AUDIO_STOP, NULL);
    set_running(s, false);
}

static bool start_node(serve_t* s)
{
    int ret = Dmod_Ioctl(s->node, DMDRVI_IOCTL_AUDIO_START, NULL);
    if (ret != 0)
    {
        DMOD_LOG_ERROR("libdmaudio: %s does not start (%d)\n", s->device->name, ret);
        return false;
    }
    set_running(s, true);
    apply_settings(s);      /* START applies the node's configuration again */
    return true;
}

/* The node configured with the options (if it is not fixed), the format it runs at */
static int configure_node(void* node, const libdmaudio_serve_options_t* o, libdmaudio_format_t* format)
{
    dmdrvi_audio_config_t config = {
        .sample_rate_hz = o->sample_rate_hz,
        .channels = o->channels,
        .sample_bits = o->sample_bits,
        .output = o->output,
        .volume_percent = o->volume_percent,
        .muted = false,
    };
    int ret = Dmod_Ioctl(node, DMDRVI_IOCTL_AUDIO_CONFIGURE, &config);
    if (ret != 0 && ret != -ENOTSUP)
        return ret;
    if (ret == -ENOTSUP)
        DMOD_LOG_INFO("libdmaudio: the node keeps its own format\n");

    dmdrvi_audio_stream_t stream;
    memset(&stream, 0, sizeof(stream));
    ret = Dmod_Ioctl(node, DMDRVI_IOCTL_AUDIO_GET_STREAM, &stream);
    if (ret != 0)
        return ret;
    format->sample_rate_hz = stream.sample_rate_hz;
    format->channels = stream.channels;
    format->format = stream.format;
    if (!stream.transmit || !format_valid(format))
        return -ENOTSUP;
    return 0;
}

dmod_libdmaudio_api_declaration(1.0, int, _serve, ( const char* name, const char* node_path, const libdmaudio_serve_options_t* options, bool (*stop_requested)(void) ))
{
    if (name == NULL || node_path == NULL || stop_requested == NULL)
        return -EINVAL;
    libdmaudio_serve_options_t o;
    if (options != NULL)
        o = *options;
    else
        libdmaudio_serve_defaults(&o);

    serve_t s;
    memset(&s, 0, sizeof(s));
    s.hw_volume = true;
    s.hw_mute = true;
    s.node = Dmod_FileOpen(node_path, "r+");
    if (s.node == NULL)
    {
        DMOD_LOG_ERROR("libdmaudio: no audio node %s\n", node_path);
        return -ENODEV;
    }

    libdmaudio_format_t format;
    int ret = configure_node(s.node, &o, &format);
    if (ret != 0)
    {
        DMOD_LOG_ERROR("libdmaudio: %s cannot play (%d)\n", node_path, ret);
        Dmod_FileClose(s.node);
        return ret;
    }
    uint32_t timeout = o.timeout_ms;
    (void)Dmod_Ioctl(s.node, DMDRVI_IOCTL_AUDIO_SET_TIMEOUT, &timeout);
    (void)Dmod_Ioctl(s.node, DMDRVI_IOCTL_AUDIO_STOP, NULL);    /* (a codec starts at CONFIGURE) - started when something plays */

    s.device = libdmaudio_device_register(name, &format);
    uint32_t period = (uint32_t)(((uint64_t)format.sample_rate_hz * o.period_ms + 999u) / 1000u);
    uint32_t bytes = period * format_bytes(format.format) * format.channels;
    void* buffer = (s.device != NULL) ? Dmod_Malloc(bytes) : NULL;
    if (s.device == NULL || buffer == NULL)
    {
        ret = (s.device == NULL) ? -EBUSY : -ENOMEM;
        DMOD_LOG_ERROR("libdmaudio: %s: %s\n", name, (ret == -EBUSY) ? "already served" : "out of memory");
        libdmaudio_device_unregister(s.device);
        Dmod_FileClose(s.node);
        return ret;
    }
    dmosi_mutex_lock(g_lock);
    s.device->volume = o.volume_percent;
    s.device->settings++;
    dmosi_mutex_unlock(g_lock);
    DMOD_LOG_INFO("libdmaudio: %s on %s - %lu Hz, %u channels, %lu-frame periods\n",
                  name, node_path, (unsigned long)format.sample_rate_hz, (unsigned)format.channels, (unsigned long)period);

    uint32_t idle = 0;
    while (!stop_requested())
    {
        if (!s.running)
        {
            if (libdmaudio_device_wait(s.device, IDLE_POLL_MS) != 0)
                continue;
            if (!start_node(&s))
            {
                dmosi_thread_sleep(IDLE_POLL_MS);
                continue;
            }
            idle = 0;
        }
        if (__atomic_load_n(&s.device->settings, __ATOMIC_RELAXED) != s.settings)
            apply_settings(&s);

        int active = libdmaudio_device_mix(s.device, buffer, period);
        idle = (active > 0) ? 0 : idle + o.period_ms;
        size_t written = Dmod_FileWrite(buffer, 1, bytes, s.node);
        if (written != bytes)
        {
            DMOD_LOG_WARN("libdmaudio: %s took %lu of %lu bytes\n", name, (unsigned long)written, (unsigned long)bytes);
            if (written == 0)
                dmosi_thread_sleep(o.period_ms);    /* (the node is stuck: no busy loop) */
        }
        if (idle >= o.idle_ms)
            stop_node(&s);
    }

    stop_node(&s);
    libdmaudio_device_unregister(s.device);
    Dmod_Free(buffer);
    Dmod_FileClose(s.node);
    return 0;
}
