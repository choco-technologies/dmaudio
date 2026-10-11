#include "dmod.h"
#include "libdmaudio.h"
#include <errno.h>
#include <string.h>

/**
 * @brief audioplay - plays a WAV file through libdmaudio.
 *
 * Usage: audioplay [-d DEVICE] [-v VOLUME] FILE.wav
 *
 * PCM of 16, 24 or 32 bits, 1 to 8 channels, at any rate: the device's
 * service converts it to what the device plays. (24-bit samples are
 * unpacked here into the 24-in-32 format of the streams.)
 */

#define BLOCK_FRAMES    512u

#define BLOCK_BYTES     (BLOCK_FRAMES * 4u * LIBDMAUDIO_MAX_CHANNELS)

static uint8_t* g_in;       /* What is read from the file - */
static uint8_t* g_out;      /* - and 24-bit samples unpacked */

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool read_all(void* f, void* buf, size_t size)
{
    return Dmod_FileRead(buf, 1, size, f) == size;
}

/* The fmt and data chunks: the format, the bits per sample in the file, the data's size */
static int parse(void* f, libdmaudio_format_t* format, uint32_t* bits, uint32_t* data_size)
{
    uint8_t h[12];
    if (!read_all(f, h, sizeof(h)) || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
        return -EBADMSG;
    bool have_fmt = false;
    for (;;)
    {
        uint8_t c[8];
        if (!read_all(f, c, sizeof(c)))
            return -EBADMSG;
        uint32_t size = le32(c + 4);
        if (memcmp(c, "fmt ", 4) == 0 && size >= 16 && size <= 64)
        {
            uint8_t fmt[64];
            if (!read_all(f, fmt, size + (size & 1u)))
                return -EBADMSG;
            uint16_t tag = le16(fmt);
            if (tag == 0xFFFEu && size >= 26)
                tag = le16(fmt + 24);                   /* WAVE_FORMAT_EXTENSIBLE: the sub-format */
            if (tag != 1u)
                return -ENOTSUP;                        /* Not PCM */
            format->channels = (uint8_t)le16(fmt + 2);
            format->sample_rate_hz = le32(fmt + 4);
            *bits = le16(fmt + 14);
            if (*bits == 16)
                format->format = DMDRVI_AUDIO_FORMAT_S16_LE;
            else if (*bits == 24)
                format->format = DMDRVI_AUDIO_FORMAT_S24_IN32_LE;
            else if (*bits == 32)
                format->format = DMDRVI_AUDIO_FORMAT_S32_LE;
            else
                return -ENOTSUP;
            if (format->channels == 0 || format->channels > LIBDMAUDIO_MAX_CHANNELS)
                return -ENOTSUP;
            have_fmt = true;
        }
        else if (memcmp(c, "data", 4) == 0)
        {
            *data_size = size;
            return have_fmt ? 0 : -EBADMSG;
        }
        else
        {
            /* Another chunk: skipped */
            for (uint32_t left = size + (size & 1u); left > 0; )
            {
                uint32_t n = (left > BLOCK_BYTES) ? BLOCK_BYTES : left;
                if (!read_all(f, g_in, n))
                    return -EBADMSG;
                left -= n;
            }
        }
    }
}

int main(int argc, char* argv[])
{
    const char* device = NULL;
    const char* path = NULL;
    uint32_t volume = 100;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            device = argv[++i];
        else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc)
        {
            volume = 0;
            for (const char* s = argv[++i]; *s >= '0' && *s <= '9'; s++)
                volume = volume * 10u + (uint32_t)(*s - '0');
            if (volume > 100)
                volume = 100;
        }
        else if (path == NULL && argv[i][0] != '-')
            path = argv[i];
        else
            path = NULL, i = argc;
    }
    if (path == NULL)
    {
        Dmod_Printf("Usage: audioplay [-d DEVICE] [-v VOLUME] FILE.wav\n");
        return -EINVAL;
    }

    g_in = Dmod_Malloc(BLOCK_BYTES);
    g_out = Dmod_Malloc(BLOCK_BYTES);
    void* f = (g_in != NULL && g_out != NULL) ? Dmod_FileOpen(path, "rb") : NULL;
    if (f == NULL)
    {
        Dmod_Printf("audioplay: cannot open %s\n", path);
        if (g_in != NULL)
            Dmod_Free(g_in);
        if (g_out != NULL)
            Dmod_Free(g_out);
        return -ENOENT;
    }
    libdmaudio_format_t format;
    uint32_t bits = 0, size = 0;
    int ret = parse(f, &format, &bits, &size);
    libdmaudio_stream_t stream = NULL;
    if (ret == 0)
    {
        Dmod_Printf("audioplay: %s - %lu Hz, %u channels, %lu bits\n", path,
                    (unsigned long)format.sample_rate_hz, (unsigned)format.channels, (unsigned long)bits);
        libdmaudio_stream_options_t options = { .buffer_ms = 0, .volume_percent = (uint8_t)volume };
        stream = libdmaudio_open(device, &format, &options, &ret);
    }
    else
        Dmod_Printf("audioplay: %s is not a PCM WAV file (%d)\n", path, ret);

    uint32_t in_frame = (bits / 8u) * (stream != NULL ? format.channels : 1u);
    while (ret == 0 && size >= in_frame)
    {
        uint32_t frames = size / in_frame;
        if (frames > BLOCK_FRAMES)
            frames = BLOCK_FRAMES;
        size_t got = Dmod_FileRead(g_in, 1, frames * in_frame, f);
        frames = (uint32_t)(got / in_frame);
        if (frames == 0)
            break;
        size -= frames * in_frame;
        const void* data = g_in;
        if (bits == 24)
        {
            /* Packed 3 bytes into the low bytes of a 32-bit word, sign extended */
            uint32_t samples = frames * format.channels;
            for (uint32_t i = 0; i < samples; i++)
            {
                g_out[4 * i] = g_in[3 * i];
                g_out[4 * i + 1] = g_in[3 * i + 1];
                g_out[4 * i + 2] = g_in[3 * i + 2];
                g_out[4 * i + 3] = (g_in[3 * i + 2] & 0x80u) ? 0xFFu : 0x00u;
            }
            data = g_out;
        }
        int32_t w = libdmaudio_write(stream, data, frames, -1);
        if (w < 0)
            ret = w;
    }
    if (ret == 0 && stream != NULL)
        (void)libdmaudio_drain(stream, 5000);
    if (stream == NULL && ret != 0 && ret != -EBADMSG && ret != -ENOTSUP)
        Dmod_Printf("audioplay: cannot open a stream on %s (%d)\n", (device != NULL) ? device : "the first device", ret);
    libdmaudio_close(stream);
    Dmod_FileClose(f);
    Dmod_Free(g_in);
    Dmod_Free(g_out);
    return ret;
}
