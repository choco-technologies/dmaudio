#include "private.h"
#include <string.h>

/*
 * Samples in and out of Q24, channels mapped on the way in:
 *  - as many channels: as they are;
 *  - one into several: the same in every channel;
 *  - several into one: their average;
 *  - otherwise the first ones in order, repeated when there are fewer
 *    (5.1 into stereo keeps front left and right).
 */

uint32_t format_bytes(dmdrvi_audio_format_t format)
{
    switch (format)
    {
        case DMDRVI_AUDIO_FORMAT_S16_LE:      return 2;
        case DMDRVI_AUDIO_FORMAT_S24_IN32_LE: return 4;
        case DMDRVI_AUDIO_FORMAT_S32_LE:      return 4;
        default:                              return 0;
    }
}

bool format_valid(const libdmaudio_format_t* format)
{
    return format != NULL &&
           format->sample_rate_hz >= 1000u && format->sample_rate_hz <= 384000u &&
           format->channels >= 1u && format->channels <= LIBDMAUDIO_MAX_CHANNELS &&
           format_bytes(format->format) != 0;
}

static int32_t read_sample(dmdrvi_audio_format_t format, const uint8_t* p)
{
    switch (format)
    {
        case DMDRVI_AUDIO_FORMAT_S16_LE:
            return (int32_t)(int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)) * 256;
        case DMDRVI_AUDIO_FORMAT_S24_IN32_LE:
        {
            uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
            return (int32_t)(v << 8) / 256;            /* Sign of bit 23 */
        }
        default:
        {
            uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            return (int32_t)v / 256;
        }
    }
}

void read_frame(const libdmaudio_format_t* from, const uint8_t* src, uint8_t to_channels, int32_t* out)
{
    uint32_t bytes = format_bytes(from->format);
    int32_t in[LIBDMAUDIO_MAX_CHANNELS];
    for (uint8_t c = 0; c < from->channels; c++)
        in[c] = read_sample(from->format, src + c * bytes);

    if (from->channels == to_channels)
    {
        memcpy(out, in, to_channels * sizeof(int32_t));
    }
    else if (to_channels == 1u)
    {
        int64_t sum = 0;
        for (uint8_t c = 0; c < from->channels; c++)
            sum += in[c];
        out[0] = (int32_t)(sum / from->channels);
    }
    else
    {
        for (uint8_t c = 0; c < to_channels; c++)
            out[c] = in[c % from->channels];
    }
}

void write_frames(const libdmaudio_format_t* to, const int32_t* in, uint32_t frames, uint8_t* dst)
{
    uint32_t samples = frames * to->channels;
    for (uint32_t i = 0; i < samples; i++)
    {
        int32_t v = in[i];
        if (v > Q24_MAX)
            v = Q24_MAX;
        else if (v < Q24_MIN)
            v = Q24_MIN;
        switch (to->format)
        {
            case DMDRVI_AUDIO_FORMAT_S16_LE:
            {
                uint16_t s = (uint16_t)(int16_t)(v >> 8);
                dst[0] = (uint8_t)s;
                dst[1] = (uint8_t)(s >> 8);
                dst += 2;
                break;
            }
            case DMDRVI_AUDIO_FORMAT_S24_IN32_LE:
            {
                uint32_t s = (uint32_t)v;
                dst[0] = (uint8_t)s;
                dst[1] = (uint8_t)(s >> 8);
                dst[2] = (uint8_t)(s >> 16);
                dst[3] = (v < 0) ? 0xFFu : 0x00u;
                dst += 4;
                break;
            }
            default:
            {
                uint32_t s = (uint32_t)v << 8;
                dst[0] = (uint8_t)s;
                dst[1] = (uint8_t)(s >> 8);
                dst[2] = (uint8_t)(s >> 16);
                dst[3] = (uint8_t)(s >> 24);
                dst += 4;
                break;
            }
        }
    }
}

/* Percent into Q15, on a square curve: half way is a quarter of the power - closer to how loud it sounds */
uint32_t gain_of(uint8_t percent)
{
    uint32_t p = (percent > 100u) ? 100u : percent;
    return (p * p * GAIN_ONE + 5000u) / 10000u;
}
