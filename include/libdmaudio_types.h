#ifndef LIBDMAUDIO_TYPES_H
#define LIBDMAUDIO_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dmdrvi_ioctl.h"

/** @brief Most channels of a stream or a device. */
#define LIBDMAUDIO_MAX_CHANNELS         8u

/** @brief Default buffer of a stream, in milliseconds of its samples. */
#define LIBDMAUDIO_DEFAULT_BUFFER_MS    200u

typedef struct libdmaudio_stream* libdmaudio_stream_t;
typedef struct libdmaudio_device* libdmaudio_device_t;

/** @brief Format of samples: rate, channels (interleaved), representation. */
typedef struct
{
    uint32_t                sample_rate_hz;     /**< Frames per second, 1000 ... 384000 */
    uint8_t                 channels;           /**< 1 ... LIBDMAUDIO_MAX_CHANNELS */
    dmdrvi_audio_format_t   format;             /**< S16_LE, S24_IN32_LE or S32_LE */
} libdmaudio_format_t;

/** @brief Options of a stream (libdmaudio_open()). */
typedef struct
{
    uint32_t    buffer_ms;          /**< Buffer, 0: LIBDMAUDIO_DEFAULT_BUFFER_MS */
    uint8_t     volume_percent;     /**< Volume, 0 ... 100 (taken as it is: 0 is silence) */
} libdmaudio_stream_options_t;

/** @brief State of a stream. */
typedef struct
{
    uint32_t    buffered_frames;    /**< Written, not mixed yet */
    uint32_t    buffer_frames;      /**< Size of the buffer */
    uint64_t    played_frames;      /**< Mixed since it was opened (its frames) */
    uint32_t    underruns;          /**< Times it ran dry while it played */
    bool        paused;
    bool        orphaned;           /**< Its device is gone */
} libdmaudio_stream_status_t;

/** @brief A device as the streams see it. */
typedef struct
{
    char                name[32];
    libdmaudio_format_t format;     /**< What the mix is made in */
    uint32_t            streams;    /**< Streams open on it */
    uint8_t             volume_percent;
    bool                muted;
    bool                running;    /**< Its node plays (not idle) */
} libdmaudio_device_info_t;

/** @brief Options of libdmaudio_serve(). */
typedef struct
{
    uint32_t                sample_rate_hz;     /**< Asked of the node */
    uint8_t                 channels;
    uint8_t                 sample_bits;        /**< 16, 24 or 32 */
    dmdrvi_audio_output_t   output;
    uint8_t                 volume_percent;     /**< Master volume at start */
    uint32_t                period_ms;          /**< Mixed and written at once */
    uint32_t                idle_ms;            /**< Silence before the node is stopped */
    uint32_t                timeout_ms;         /**< Longest write to the node */
} libdmaudio_serve_options_t;

#endif /* LIBDMAUDIO_TYPES_H */
