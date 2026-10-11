#include "dmod.h"
#include "libdmaudio.h"
#include "libsystemd.h"
#include <errno.h>

/**
 * @brief dmaudio - the audio service: plays the mix of the streams of one
 *        audio device.
 *
 * Usage: dmaudio <device name> [<audio node>] [options of libdmaudio_serve_parse()]
 *
 * Started per device by the device rule `[class=audio] start=dmaudio@%name`
 * (configs/dmaudio.rules) with the device's name and node path. Everything
 * is libdmaudio's (libdmaudio_serve()) - one library for all the services.
 */
int main(int argc, char* argv[])
{
    if (argc < 2 || argv[1][0] == '\0')
    {
        Dmod_Printf("Usage: dmaudio <device name> [<audio node>] [-r RATE] [-c CHANNELS] [-b BITS]\n"
                    "       [-o headphone|speaker|line] [-v VOLUME] [-p PERIOD_MS] [-i IDLE_MS] [-t TIMEOUT_MS]\n"
                    "Started per device from dmaudio@.ini - see dmaudio.rules\n");
        return -EINVAL;
    }
    char node[64];
    int first = 2;
    if (argc > 2 && argv[2][0] == '/')
        Dmod_SnPrintf(node, sizeof(node), "%s", argv[first++]);
    else
        Dmod_SnPrintf(node, sizeof(node), "/dev/%s", argv[1]);

    libdmaudio_serve_options_t options;
    libdmaudio_serve_defaults(&options);
    int ret = libdmaudio_serve_parse(argc - first, &argv[first], &options);
    return (ret == 0) ? libdmaudio_serve(argv[1], node, &options, libsystemd_stop_requested) : ret;
}
