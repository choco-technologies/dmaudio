#ifndef LIBDMAUDIO_H
#define LIBDMAUDIO_H

#include "dmod_types.h"
#include "libdmaudio_defs.h"
#include "libdmaudio_types.h"

/**
 * libdmaudio - the sound of the system: applications play streams, the
 * library mixes them for each audio device.
 *
 * An application opens a stream on a device (libdmaudio_open()) in the
 * format of its samples - any rate, 1 to 8 channels, 16, 24 or 32-bit -
 * and writes into it. The stream is a buffer of its own: the application
 * writes when it is scheduled, in pieces of any size, and the device takes
 * the samples at its own pace - converted to the device's rate, channels
 * and sample format, with the stream's volume, mixed with the other
 * streams.
 *
 * Each device is served by one thread (the dmaudio service,
 * libdmaudio_serve()): it mixes a period, writes it to the device's node,
 * and so on - the node's DMA paces it. While nothing plays the device is
 * stopped.
 *
 * The library is loaded once in the system: everything is here, the
 * services only call libdmaudio_serve().
 */

/* ---- Streams (applications) ---- */

/**
 * @brief Open a playback stream on a device.
 *
 * @param device  Name of the device (its node name, e.g. "wm8994"), NULL for
 *                the first one served
 * @param format  Format of the samples the stream is written with
 * @param options Buffering and volume, NULL for the defaults
 * @param status  Receives 0, -ENODEV (no such device), -EINVAL (a format
 *                that is not supported) or -ENOMEM (may be NULL)
 * @return The stream, NULL on failure
 */
dmod_libdmaudio_api(1.0, libdmaudio_stream_t, _open, ( const char* device, const libdmaudio_format_t* format, const libdmaudio_stream_options_t* options, int* status ));

/**
 * @brief Close a stream: what is still buffered is dropped (see
 *        libdmaudio_drain()). Safe on NULL.
 */
dmod_libdmaudio_api(1.0, void, _close, ( libdmaudio_stream_t stream ));

/**
 * @brief Write frames into a stream.
 *
 * Waits for room in the stream's buffer while it is full, at most
 * @p timeout_ms (0: does not wait, -1: no limit).
 *
 * @param frames Frames (one sample of every channel) in @p data
 * @return Frames written - fewer when the time ran out -, -ETIMEDOUT when
 *         none could be, -ENODEV when the device is gone, -EINVAL
 */
dmod_libdmaudio_api(1.0, int32_t, _write, ( libdmaudio_stream_t stream, const void* data, uint32_t frames, int32_t timeout_ms ));

/**
 * @brief Wait until everything written into the stream has been mixed.
 *
 * @return 0, -ETIMEDOUT, -ENODEV when the device is gone, -EINVAL
 */
dmod_libdmaudio_api(1.0, int, _drain, ( libdmaudio_stream_t stream, int32_t timeout_ms ));

/** @brief Volume of a stream, 0 ... 100 (100: as written). */
dmod_libdmaudio_api(1.0, int, _set_volume, ( libdmaudio_stream_t stream, uint8_t volume_percent ));

/** @brief Pause a stream (it keeps what it holds) or resume it. */
dmod_libdmaudio_api(1.0, int, _set_paused, ( libdmaudio_stream_t stream, bool paused ));

/** @brief State of a stream: what it holds, underruns, ... */
dmod_libdmaudio_api(1.0, int, _get_status, ( libdmaudio_stream_t stream, libdmaudio_stream_status_t* status ));

/* ---- Devices ---- */

/**
 * @brief Information about a device that is served.
 *
 * @param device NULL for the first one served
 * @return 0, -ENODEV
 */
dmod_libdmaudio_api(1.0, int, _device_info, ( const char* device, libdmaudio_device_info_t* info ));

/**
 * @brief Name of the @p index-th device served (0: the first one).
 *
 * @return 0, -ENOENT when there are fewer, -ERANGE when @p name is too small
 */
dmod_libdmaudio_api(1.0, int, _device_name, ( uint32_t index, char* name, size_t size ));

/**
 * @brief Master volume of a device, 0 ... 100: its node's
 *        DMDRVI_IOCTL_AUDIO_SET_VOLUME, or applied to the mix when the node
 *        has no volume.
 */
dmod_libdmaudio_api(1.0, int, _device_set_volume, ( const char* device, uint8_t volume_percent ));

/** @brief Mute a device or turn it up again. */
dmod_libdmaudio_api(1.0, int, _device_set_mute, ( const char* device, bool muted ));

/* ---- Serving a device (the dmaudio service) ---- */

/** @brief Default options of libdmaudio_serve(). */
dmod_libdmaudio_api(1.0, void, _serve_defaults, ( libdmaudio_serve_options_t* options ));

/**
 * @brief Options of libdmaudio_serve() from command line arguments
 *        (-r RATE, -c CHANNELS, -b BITS, -o headphone|speaker|line,
 *        -v VOLUME, -p PERIOD_MS, -i IDLE_MS), on top of @p options.
 *
 * @return 0, -EINVAL (an unknown or bad option, reported)
 */
dmod_libdmaudio_api(1.0, int, _serve_parse, ( int argc, char* argv[], libdmaudio_serve_options_t* options ));

/**
 * @brief Serve a device: play the mix of its streams through its node until
 *        @p stop_requested returns true.
 *
 * The node is configured (DMDRVI_IOCTL_AUDIO_CONFIGURE) with the options'
 * format - a node with a fixed format (a bare serial audio interface) keeps
 * its own -, the device registered under @p name in the format the node
 * reports (DMDRVI_IOCTL_AUDIO_GET_STREAM). Then, a period at a time, the
 * streams are mixed and written to the node, whose DMA paces the loop.
 * After `idle_ms` with nothing to play the node is drained and stopped
 * until a stream has samples again.
 *
 * @param name           Device name the streams are opened with
 * @param node_path      Its audio node (e.g. "/dev/wm8994")
 * @param options        NULL for the defaults
 * @param stop_requested Asked every period and while idle (every 100 ms)
 * @return 0 when stopped, -ENODEV (no node), -EBUSY (the name is served),
 *         -ENOTSUP (no playback, or a format the library cannot write), or
 *         the node's error
 */
dmod_libdmaudio_api(1.0, int, _serve, ( const char* name, const char* node_path, const libdmaudio_serve_options_t* options, bool (*stop_requested)(void) ));

/* ---- The mixer, for a service of its own ---- */

/**
 * @brief Register a device without libdmaudio_serve() - for a service that
 *        moves the samples itself (and for tests).
 *
 * @return The registration, NULL when the name is taken, the format is not
 *         supported or out of memory
 */
dmod_libdmaudio_api(1.0, libdmaudio_device_t, _device_register, ( const char* name, const libdmaudio_format_t* format ));

/** @brief Unregister a device: its streams fail with -ENODEV. Safe on NULL. */
dmod_libdmaudio_api(1.0, void, _device_unregister, ( libdmaudio_device_t device ));

/**
 * @brief Mix @p frames frames of the device's streams into @p out (the
 *        device's format); silence where nothing plays.
 *
 * @return Streams that had samples in it (0: all silence), or -EINVAL
 */
dmod_libdmaudio_api(1.0, int, _device_mix, ( libdmaudio_device_t device, void* out, uint32_t frames ));

/**
 * @brief Wait until a stream of the device has samples (or @p timeout_ms).
 *
 * @return 0 when one has, -ETIMEDOUT
 */
dmod_libdmaudio_api(1.0, int, _device_wait, ( libdmaudio_device_t device, int32_t timeout_ms ));

#endif /* LIBDMAUDIO_H */
