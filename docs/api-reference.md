# libdmaudio API Reference

```c
#include "libdmaudio.h"     /* includes libdmaudio_types.h, dmdrvi_ioctl.h */
```

Errors are negative `errno` values.

## Types

| Type | Meaning |
|------|---------|
| `libdmaudio_format_t` | `sample_rate_hz` (1000 ... 384000), `channels` (1 ... `LIBDMAUDIO_MAX_CHANNELS` = 8, interleaved), `format` (`DMDRVI_AUDIO_FORMAT_S16_LE`, `_S24_IN32_LE`, `_S32_LE`) |
| `libdmaudio_stream_options_t` | `buffer_ms` (0: `LIBDMAUDIO_DEFAULT_BUFFER_MS` = 200), `volume_percent` |
| `libdmaudio_stream_status_t` | `buffered_frames`, `buffer_frames`, `played_frames`, `underruns`, `paused`, `orphaned` |
| `libdmaudio_device_info_t` | `name`, `format` (of the mix), `streams`, `volume_percent`, `muted`, `running` |
| `libdmaudio_serve_options_t` | what `libdmaudio_serve()` asks of the node, `period_ms`, `idle_ms`, `timeout_ms` |

## Streams

### `libdmaudio_open(device, format, options, status)`

Opens a playback stream. `device` is a device name (the node name, e.g.
`"wm8994"`) or `NULL` for the first one served; `options` `NULL` is a
200 ms buffer at volume 100 - given options are taken as they are (their
`volume_percent` 0 is silence). `status`
(may be `NULL`) receives 0, `-ENODEV` (no such device - also while its
service is still starting), `-EINVAL` (a format outside the table above) or
`-ENOMEM`.

### `libdmaudio_write(stream, data, frames, timeout_ms)`

Copies `frames` frames into the stream's buffer, waiting for room while it
is full - at most `timeout_ms` (0: does not wait, -1: no limit). Returns the
frames written (fewer when the time ran out), `-ETIMEDOUT` when none could
be, `-ENODEV` when the device is gone.

### `libdmaudio_drain(stream, timeout_ms)`

Waits until the mixer has taken everything written. 0, `-ETIMEDOUT`,
`-ENODEV`. What the mixer took is in the node's buffers - closing right
after a drain does not cut it.

### `libdmaudio_close(stream)`

Closes it; what it still holds is dropped. A stream of a process that ends
without closing it is dropped by the mixer within a second.

### `libdmaudio_set_volume(stream, percent)`, `libdmaudio_set_paused(stream, paused)`, `libdmaudio_get_status(stream, &status)`

Volume 0 ... 100 on a square curve (gain = (percent / 100)^2). A paused
stream keeps what it holds and is not mixed. `underruns` counts the times
the stream ran dry while it was playing (a stream that has not been written
yet is not running dry).

## Devices

| Function | |
|----------|-|
| `libdmaudio_device_info(device, &info)` | `device` NULL: the first; `-ENODEV` |
| `libdmaudio_device_name(index, name, size)` | the `index`-th device served; `-ENOENT` past the last, `-ERANGE` |
| `libdmaudio_device_set_volume(device, percent)` | master volume - the node's `AUDIO_SET_VOLUME`, or the mix |
| `libdmaudio_device_set_mute(device, muted)` | the node's `AUDIO_SET_MUTE`, or the mix |

## Serving a device

### `libdmaudio_serve(name, node_path, options, stop_requested)`

What the `dmaudio` service does: opens the node, configures it
(`AUDIO_CONFIGURE` - a node with a fixed format answers `-ENOTSUP` and keeps
its own), takes its format from `AUDIO_GET_STREAM`, registers the device
`name` in it and then, until `stop_requested()` is true:

1. while it is idle, waits (`libdmaudio_device_wait()`, 100 ms at a time)
   for a stream with samples, and starts the node (`AUDIO_START`, then the
   master volume and mute again);
2. mixes a period (`libdmaudio_device_mix()`) and writes it to the node,
   which waits for room in its DMA buffers;
3. after `idle_ms` of silence drains (`AUDIO_DRAIN`) and stops the node.

Returns 0 when stopped, `-ENODEV` (no node), `-EBUSY` (the name is served),
`-ENOTSUP` (the node does not play, or reports a format the mixer cannot
write) or the node's error.

`libdmaudio_serve_defaults()`: 48000 Hz, 2 channels, 16 bits, headphone,
volume 70, 10 ms periods, 2000 ms idle, 1000 ms write timeout.
`libdmaudio_serve_parse(argc, argv, &options)` reads `-r -c -b -o -v -p -i -t`.

## The mixer, for a service of its own

`libdmaudio_device_register(name, format)` / `libdmaudio_device_unregister()`
register a device without a node - for a service that moves the samples
itself, and for the tests. `libdmaudio_device_mix(device, out, frames)`
fills `out` (the device's format) with the mix and returns how many streams
had samples in it (0: silence). `libdmaudio_device_wait(device, timeout_ms)`
waits for a stream with samples. Streams of a device that is unregistered
fail with `-ENODEV` until they are closed.

## Tools

| Command | |
|---------|-|
| `dmaudio <name> [<node>] [options]` | the service (from `dmaudio@.ini`) |
| `audiotone [-d DEVICE] [-f HZ]... [-t SECONDS] [-r RATE] [-v VOLUME]` | tones, each in a stream of its own, made at RATE (22050) mono |
| `audioplay [-d DEVICE] [-v VOLUME] FILE.wav` | a PCM WAV file |
| `audioctl [list]`, `audioctl volume [DEVICE] PERCENT`, `audioctl mute\|unmute [DEVICE]` | the devices served |
