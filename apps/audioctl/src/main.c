#include "dmod.h"
#include "libdmaudio.h"
#include <errno.h>
#include <string.h>

/**
 * @brief audioctl - the audio devices libdmaudio serves.
 *
 * Usage: audioctl [list]
 *        audioctl volume [DEVICE] PERCENT
 *        audioctl mute|unmute [DEVICE]
 *        audioctl output [DEVICE] headphone|speaker|line
 */


static const char* format_name(dmdrvi_audio_format_t f)
{
    switch (f)
    {
        case DMDRVI_AUDIO_FORMAT_S16_LE:      return "s16";
        case DMDRVI_AUDIO_FORMAT_S24_IN32_LE: return "s24";
        case DMDRVI_AUDIO_FORMAT_S32_LE:      return "s32";
        default:                              return "?";
    }
}

/* (a switch, not a table of strings: a module's data is not relocated) */
static const char* output_name(dmdrvi_audio_output_t o)
{
    switch (o)
    {
        case DMDRVI_AUDIO_OUTPUT_HEADPHONE: return "headphone";
        case DMDRVI_AUDIO_OUTPUT_SPEAKER:   return "speaker";
        case DMDRVI_AUDIO_OUTPUT_LINE:      return "line";
        default:                            return "?";
    }
}

static int list(void)
{
    char name[32];
    uint32_t i = 0;
    for (; libdmaudio_device_name(i, name, sizeof(name)) == 0; i++)
    {
        libdmaudio_device_info_t info;
        if (libdmaudio_device_info(name, &info) != 0)
            continue;
        Dmod_Printf("%-12s %6lu Hz %u ch %s  %-9s  volume %3u%%%s  %lu stream(s)  %s\n", info.name,
                    (unsigned long)info.format.sample_rate_hz, (unsigned)info.format.channels,
                    format_name(info.format.format), output_name(info.output), (unsigned)info.volume_percent,
                    info.muted ? " muted" : "", (unsigned long)info.streams, info.running ? "playing" : "idle");
    }
    if (i == 0)
        Dmod_Printf("audioctl: no audio device is served (is dmaudio running?)\n");
    return 0;
}

int main(int argc, char* argv[])
{
    const char* cmd = (argc > 1) ? argv[1] : "list";
    int ret = -EINVAL;
    if (strcmp(cmd, "list") == 0 && argc <= 2)
        return list();
    if (strcmp(cmd, "volume") == 0 && (argc == 3 || argc == 4))
    {
        const char* value = argv[argc - 1];
        uint32_t v = 0;
        bool ok = *value != '\0';
        for (const char* s = value; *s != '\0'; s++)
        {
            ok = ok && *s >= '0' && *s <= '9';
            v = v * 10u + (uint32_t)(*s - '0');
        }
        if (ok && v <= 100)
            ret = libdmaudio_device_set_volume((argc == 4) ? argv[2] : NULL, (uint8_t)v);
    }
    else if ((strcmp(cmd, "mute") == 0 || strcmp(cmd, "unmute") == 0) && argc <= 3)
        ret = libdmaudio_device_set_mute((argc == 3) ? argv[2] : NULL, cmd[0] == 'm');
    else if (strcmp(cmd, "output") == 0 && (argc == 3 || argc == 4))
    {
        for (uint32_t i = 0; i <= (uint32_t)DMDRVI_AUDIO_OUTPUT_LINE; i++)
            if (strcmp(argv[argc - 1], output_name((dmdrvi_audio_output_t)i)) == 0)
                ret = libdmaudio_device_set_output((argc == 4) ? argv[2] : NULL, (dmdrvi_audio_output_t)i);
    }
    else
    {
        Dmod_Printf("Usage: audioctl [list]\n"
                    "       audioctl volume [DEVICE] PERCENT\n"
                    "       audioctl mute|unmute [DEVICE]\n"
                    "       audioctl output [DEVICE] headphone|speaker|line\n");
        return -EINVAL;
    }
    if (ret == -ENODEV)
        Dmod_Printf("audioctl: no such audio device\n");
    return ret;
}
