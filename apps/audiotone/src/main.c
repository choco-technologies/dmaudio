#include "dmod.h"
#include "libdmaudio.h"
#include <errno.h>
#include <string.h>

/**
 * @brief audiotone - plays sine tones through libdmaudio.
 *
 * Usage: audiotone [-d DEVICE] [-f HZ]... [-t SECONDS] [-r RATE] [-v VOLUME]
 *
 * Each -f is a tone of its own stream (up to 4) - they are mixed by the
 * device's service. The tones are made at RATE (default 22050, mono), so
 * they are converted to the device's rate and channels on the way.
 */

#define MAX_TONES   4u
#define CHUNK       256u

typedef struct
{
    libdmaudio_stream_t stream;
    float               k;          /* 2 cos(w) */
    float               y1, y2;     /* The last two samples */
} tone_t;

static int16_t g_pcm[CHUNK];

/* cos and sin of a small angle (w < pi) by their series - no libm */
static float cos_of(float w)
{
    float w2 = w * w, term = 1.0f, sum = 1.0f;
    for (int n = 1; n < 12; n++)
    {
        term *= -w2 / (float)((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sum;
}

static float sin_of(float w)
{
    float w2 = w * w, term = w, sum = w;
    for (int n = 1; n < 12; n++)
    {
        term *= -w2 / (float)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

static bool number(const char* s, uint32_t* out)
{
    uint32_t v = 0;
    if (s == NULL || *s == '\0')
        return false;
    for (; *s != '\0'; s++)
    {
        if (*s < '0' || *s > '9' || v > 10000000u)
            return false;
        v = v * 10u + (uint32_t)(*s - '0');
    }
    *out = v;
    return true;
}

int main(int argc, char* argv[])
{
    const char* device = NULL;
    uint32_t freqs[MAX_TONES];
    uint32_t count = 0, seconds = 2, rate = 22050, volume = 50;
    for (int i = 1; i < argc; i++)
    {
        const char* val = (i + 1 < argc) ? argv[i + 1] : NULL;
        uint32_t v = 0;
        bool ok = val != NULL;
        if (ok && strcmp(argv[i], "-d") == 0)
            device = val;
        else if (ok && strcmp(argv[i], "-f") == 0 && count < MAX_TONES && (ok = number(val, &v)) && v >= 20 && v <= 20000)
            freqs[count++] = v;
        else if (ok && strcmp(argv[i], "-t") == 0 && (ok = number(val, &v)) && v <= 3600)
            seconds = v;
        else if (ok && strcmp(argv[i], "-r") == 0 && (ok = number(val, &v)) && v >= 8000 && v <= 192000)
            rate = v;
        else if (ok && strcmp(argv[i], "-v") == 0 && (ok = number(val, &v)) && v <= 100)
            volume = v;
        else
            ok = false;
        if (!ok)
        {
            Dmod_Printf("Usage: audiotone [-d DEVICE] [-f HZ]... [-t SECONDS] [-r RATE] [-v VOLUME]\n");
            return -EINVAL;
        }
        i++;
    }
    if (count == 0)
        freqs[count++] = 440;

    tone_t tones[MAX_TONES];
    memset(tones, 0, sizeof(tones));
    libdmaudio_format_t format = { .sample_rate_hz = rate, .channels = 1, .format = DMDRVI_AUDIO_FORMAT_S16_LE };
    libdmaudio_stream_options_t options = { .buffer_ms = 0, .volume_percent = (uint8_t)volume };
    int ret = 0;
    for (uint32_t t = 0; t < count && ret == 0; t++)
    {
        float w = 6.2831853f * (float)freqs[t] / (float)rate;
        tones[t].k = 2.0f * cos_of(w);
        tones[t].y1 = 0.0f;
        tones[t].y2 = -sin_of(w);           /* y[-1] = sin(-w) */
        tones[t].stream = libdmaudio_open(device, &format, &options, &ret);
        if (tones[t].stream == NULL)
            Dmod_Printf("audiotone: cannot open a stream on %s (%d)\n", (device != NULL) ? device : "the first device", ret);
    }

    uint32_t left = seconds * rate;
    float amplitude = 30000.0f / (float)count;
    while (ret == 0 && left > 0)
    {
        uint32_t n = (left > CHUNK) ? CHUNK : left;
        for (uint32_t t = 0; t < count && ret == 0; t++)
        {
            tone_t* x = &tones[t];
            for (uint32_t i = 0; i < n; i++)
            {
                float y = x->k * x->y1 - x->y2;
                x->y2 = x->y1;
                x->y1 = y;
                g_pcm[i] = (int16_t)(y * amplitude);
            }
            int32_t w = libdmaudio_write(x->stream, g_pcm, n, -1);
            if (w < 0)
                ret = w;
        }
        left -= n;
    }
    for (uint32_t t = 0; t < count; t++)
    {
        if (ret == 0)
            (void)libdmaudio_drain(tones[t].stream, 2000);
        libdmaudio_stream_status_t st;
        if (libdmaudio_get_status(tones[t].stream, &st) == 0)
            Dmod_Printf("audiotone: %lu Hz - %lu frames played, %lu underruns\n", (unsigned long)freqs[t],
                        (unsigned long)st.played_frames, (unsigned long)st.underruns);
        libdmaudio_close(tones[t].stream);
    }
    if (ret != 0)
        Dmod_Printf("audiotone: %d\n", ret);
    return ret;
}
