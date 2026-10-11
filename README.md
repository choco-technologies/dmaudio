# dmaudio

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmaudio/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmaudio/actions/workflows/ci.yml)

The sound of dmodOS: applications play streams, the library mixes them for
each audio device.

| Module | Kind | What it does |
|--------|------|--------------|
| `libdmaudio` | Library | Streams, conversion, the mixer, serving a device - everything |
| `dmaudio` | Service ([apps/dmaudio](apps/dmaudio)) | One instance per audio device: `libdmaudio_serve()` |
| `audiotone` | Tool | Sine tones, one stream each |
| `audioplay` | Tool | A WAV file (PCM, 16/24/32 bits, any rate, 1-8 channels) |
| `audioctl` | Tool | The devices served: list, volume, mute |

The library is loaded once in the system and holds everything; the service
is `main()` calling `libdmaudio_serve()` (under 1 kB) - there may be many
services, one per device.

## How it works

```
 application ──libdmaudio_write()──► stream buffer ─┐
 application ──libdmaudio_write()──► stream buffer ─┼─ mixer (dmaudio@<device>) ──write()──► codec / SAI node ──DMA──► DAC
 application ──libdmaudio_write()──► stream buffer ─┘
```

- **Streams.** An application opens a stream on a device (by name, or the
  first one) in the format of its own samples: any rate (1 kHz ... 384 kHz),
  1 to 8 channels, S16, S24 (in 32 bits) or S32. It writes when it is
  scheduled, in pieces of any size; the stream's buffer (200 ms by default)
  takes up the difference between when it runs and when the device needs
  the samples. A write waits while the buffer is full - that is what paces
  an application. Streams of processes that end without closing them are
  dropped.
- **Conversion.** Every stream is converted to the device's format on the
  way into the mix: its rate by linear interpolation, its channels (mono
  into all of them, several into one by their average, others in order) and
  its sample format. Mixed in 24 bits, clamped when written out.
- **Volume.** Per stream (0 ... 100 on a square curve - 50 is a quarter of
  the power), and per device: the node's `AUDIO_SET_VOLUME` / `SET_MUTE`, or
  on the mix when the node has none.
- **Decoupled from the scheduler.** The service's thread runs above the
  applications (priority 3): it mixes a period (10 ms), writes it to the
  node and waits for room in the node's DMA buffers - the node's clock paces
  it, not a timer. The buffers between the application and the mixer have
  no lock (one writer, one reader); a stream that runs dry is silence and
  counts an underrun, the others go on.
- **Idle.** With nothing to play for 2 s the node is drained and stopped;
  the first samples of a stream start it again.

## Playing

```c
#include "libdmaudio.h"

libdmaudio_format_t format = { 22050, 1, DMDRVI_AUDIO_FORMAT_S16_LE };
libdmaudio_stream_t s = libdmaudio_open(NULL, &format, NULL, NULL);   /* NULL: the first device */
libdmaudio_write(s, samples, frames, -1);   /* waits for room while the buffer is full */
libdmaudio_drain(s, 1000);
libdmaudio_close(s);
```

See [docs/api-reference.md](docs/api-reference.md).

## The service

dmdevfs reports every node answering `DMDRVI_IOCTL_AUDIO_GET_INFO` (a codec,
e.g. dmwm8994) to libsystemd as `audio`; [configs/dmaudio.rules](configs/dmaudio.rules)
starts `dmaudio@<name>` ([configs/dmaudio@.ini](configs/dmaudio@.ini)) for
it. In a firmware's `flash.dmd`:

```
dmaudio service=dmaudio@.ini
dmaudio rules=dmaudio.rules
```

The node is configured with 48 kHz, stereo, 16 bits, headphone output and
volume 70 %, which the unit's `args=` can change (`-r RATE -c CHANNELS
-b BITS -o headphone|speaker|line -v VOLUME -p PERIOD_MS -i IDLE_MS
-t TIMEOUT_MS`). A node with a fixed format (a bare SAI) keeps its own; the
mix is made in whatever the node reports (`AUDIO_GET_STREAM`).

On the STM32F746G-DISCO (with dmsai and dmwm8994):

```
stm32f746xg@/> audioctl
wm8994        48000 Hz 2 ch s16  volume  70%  0 stream(s)  idle
stm32f746xg@/> audiotone -f 523 -f 659 -t 2
audiotone: 523 Hz - 44100 frames played, 0 underruns
audiotone: 659 Hz - 44100 frames played, 0 underruns
stm32f746xg@/> audioplay /mnt/dmsdio0_0p1/music.wav
```

Not yet: capture (recording streams), and resampling better than linear
interpolation.

## Building

```bash
cmake -S . -B build -DDMOD_TOOLS_NAME=arch/armv7/cortex-m7
cmake --build build
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout.

## Testing

```bash
cmake -S . -B build
cmake --build build
cd build && ctest --output-on-failure
```

`test_libdmaudio` registers a device without a node and mixes its streams
with `libdmaudio_device_mix()`: conversions, volume, sums and clamping,
resampling, the buffers, drain, underruns, pause, devices that go away.

## Project Structure

```
dmaudio/
├── include/libdmaudio.h        # the API
├── include/libdmaudio_types.h
├── src/registry.c              # devices, streams, their buffers
├── src/mix.c                   # the mixer
├── src/convert.c               # samples, channels, volume
├── src/serve.c                 # serving a device's node
├── apps/dmaudio/               # the service
├── apps/audiotone/             # tools
├── apps/audioplay/
├── apps/audioctl/
├── configs/                    # dmaudio@.ini, dmaudio.rules
├── docs/
└── tests/
```

## License

MIT - see [LICENSE](LICENSE).
