#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "libdmaudio.h"
#include <errno.h>
#include <string.h>

/*
 * The mixer without a node: a device registered by the test
 * (libdmaudio_device_register()), its streams mixed with
 * libdmaudio_device_mix() - conversions, volume, sums, the buffers.
 */

static const libdmaudio_format_t STEREO16_48K = { 48000, 2, DMDRVI_AUDIO_FORMAT_S16_LE };
static const libdmaudio_format_t MONO16_48K   = { 48000, 1, DMDRVI_AUDIO_FORMAT_S16_LE };

static int16_t g_in[1024];
static int16_t g_out[1024];

static libdmaudio_device_t g_dev;

static libdmaudio_device_t device(const libdmaudio_format_t* format)
{
    libdmaudio_device_unregister(g_dev);
    g_dev = libdmaudio_device_register("test", format);
    return g_dev;
}

void dmod_test_teardown(void)
{
    libdmaudio_device_unregister(g_dev);
    g_dev = NULL;
}

static libdmaudio_stream_t open_stream(const libdmaudio_format_t* format, uint8_t volume)
{
    libdmaudio_stream_options_t options = { .buffer_ms = 0, .volume_percent = volume };
    int status = -1;
    libdmaudio_stream_t s = libdmaudio_open("test", format, &options, &status);
    return (status == 0) ? s : NULL;
}

DMOD_TEST_STEP(same_format_is_copied)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&STEREO16_48K));
    libdmaudio_stream_t s = open_stream(&STEREO16_48K, 100);
    DMOD_TEST_EXPECT_NOT_NULL(s);
    for (int i = 0; i < 64; i++)
        g_in[i] = (int16_t)(i * 300 - 9000);
    DMOD_TEST_EXPECT_EQ(libdmaudio_write(s, g_in, 32, 0), 32);
    DMOD_TEST_EXPECT_EQ(libdmaudio_device_mix(g_dev, g_out, 32), 1);
    DMOD_TEST_EXPECT_TRUE(memcmp(g_in, g_out, 64 * sizeof(int16_t)) == 0);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(the_ring_wraps_and_runs_dry)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&STEREO16_48K));
    libdmaudio_stream_options_t options = { .buffer_ms = 1, .volume_percent = 100 };   /* 48 frames */
    int status = -1;
    libdmaudio_stream_t s = libdmaudio_open("test", &STEREO16_48K, &options, &status);
    DMOD_TEST_EXPECT_NOT_NULL(s);
    bool same = true;
    for (int round = 0; round < 10; round++)
    {
        /* 40 frames at a time: they go round the ring's end */
        for (int i = 0; i < 80; i++)
            g_in[i] = (int16_t)(round * 1000 + i * 7 - 300);
        DMOD_TEST_EXPECT_EQ(libdmaudio_write(s, g_in, 40, 0), 40);
        DMOD_TEST_EXPECT_EQ(libdmaudio_device_mix(g_dev, g_out, 40), 1);
        same = same && memcmp(g_in, g_out, 80 * sizeof(int16_t)) == 0;
    }
    DMOD_TEST_EXPECT_TRUE(same);

    /* 10 frames, 30 asked: the rest is silence, one underrun */
    DMOD_TEST_EXPECT_EQ(libdmaudio_write(s, g_in, 10, 0), 10);
    DMOD_TEST_EXPECT_EQ(libdmaudio_device_mix(g_dev, g_out, 30), 1);
    bool silent = true;
    for (int i = 20; i < 60; i++)
        silent = silent && g_out[i] == 0;
    DMOD_TEST_EXPECT_TRUE(memcmp(g_in, g_out, 20 * sizeof(int16_t)) == 0);
    DMOD_TEST_EXPECT_TRUE(silent);
    libdmaudio_stream_status_t st;
    DMOD_TEST_EXPECT_EQ(libdmaudio_get_status(s, &st), 0);
    DMOD_TEST_EXPECT_EQ(st.underruns, 1u);
    DMOD_TEST_EXPECT_EQ(st.played_frames, 410u);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(silence_without_streams)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&STEREO16_48K));
    memset(g_out, 0x55, sizeof(g_out));
    DMOD_TEST_EXPECT_EQ(libdmaudio_device_mix(g_dev, g_out, 100), 0);
    bool silent = true;
    for (int i = 0; i < 200; i++)
        silent = silent && g_out[i] == 0;
    DMOD_TEST_EXPECT_TRUE(silent);
}

DMOD_TEST_STEP(mono_into_stereo)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&STEREO16_48K));
    libdmaudio_stream_t s = open_stream(&MONO16_48K, 100);
    for (int i = 0; i < 16; i++)
        g_in[i] = (int16_t)(i * 1000);
    DMOD_TEST_EXPECT_EQ(libdmaudio_write(s, g_in, 16, 0), 16);
    libdmaudio_device_mix(g_dev, g_out, 16);
    bool ok = true;
    for (int i = 0; i < 16; i++)
        ok = ok && g_out[2 * i] == g_in[i] && g_out[2 * i + 1] == g_in[i];
    DMOD_TEST_EXPECT_TRUE(ok);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(stereo_into_mono)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&MONO16_48K));
    libdmaudio_stream_t s = open_stream(&STEREO16_48K, 100);
    int16_t frames[4] = { 1000, 3000, -2000, -4000 };
    DMOD_TEST_EXPECT_EQ(libdmaudio_write(s, frames, 2, 0), 2);
    libdmaudio_device_mix(g_dev, g_out, 2);
    DMOD_TEST_EXPECT_EQ(g_out[0], 2000);
    DMOD_TEST_EXPECT_EQ(g_out[1], -3000);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(streams_are_summed_and_clamped)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&MONO16_48K));
    libdmaudio_stream_t a = open_stream(&MONO16_48K, 100);
    libdmaudio_stream_t b = open_stream(&MONO16_48K, 100);
    int16_t va[3] = { 1000, 30000, -30000 };
    int16_t vb[3] = { 234, 30000, -30000 };
    libdmaudio_write(a, va, 3, 0);
    libdmaudio_write(b, vb, 3, 0);
    DMOD_TEST_EXPECT_EQ(libdmaudio_device_mix(g_dev, g_out, 3), 2);
    DMOD_TEST_EXPECT_EQ(g_out[0], 1234);
    DMOD_TEST_EXPECT_EQ(g_out[1], 32767);
    DMOD_TEST_EXPECT_EQ(g_out[2], -32768);
    libdmaudio_close(a);
    libdmaudio_close(b);
}

DMOD_TEST_STEP(volume_scales_on_a_square_curve)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&MONO16_48K));
    libdmaudio_stream_t s = open_stream(&MONO16_48K, 50);
    int16_t v[2] = { 20000, -20000 };
    libdmaudio_write(s, v, 1, 0);
    libdmaudio_device_mix(g_dev, g_out, 1);
    DMOD_TEST_EXPECT_EQ(g_out[0], 5000);            /* 50 %: a quarter */
    DMOD_TEST_EXPECT_EQ(libdmaudio_set_volume(s, 0), 0);
    libdmaudio_write(s, v + 1, 1, 0);
    libdmaudio_device_mix(g_dev, g_out, 1);
    DMOD_TEST_EXPECT_EQ(g_out[0], 0);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(half_the_rate_is_interpolated)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&MONO16_48K));
    libdmaudio_format_t f = { 24000, 1, DMDRVI_AUDIO_FORMAT_S16_LE };
    libdmaudio_stream_t s = open_stream(&f, 100);
    for (int i = 0; i < 50; i++)
        g_in[i] = (int16_t)(i * 200);
    DMOD_TEST_EXPECT_EQ(libdmaudio_write(s, g_in, 50, 0), 50);
    libdmaudio_device_mix(g_dev, g_out, 96);
    /* Two device frames per source frame: the source values and the halves between them */
    bool ok = true;
    for (int i = 4; i < 90; i++)
        ok = ok && (g_out[i + 1] - g_out[i]) == 100;
    DMOD_TEST_EXPECT_TRUE(ok);
    libdmaudio_stream_status_t st;
    libdmaudio_get_status(s, &st);
    DMOD_TEST_EXPECT_EQ(st.played_frames, 48u);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(device_formats_s24_and_s32)
{
    libdmaudio_format_t f24 = { 48000, 1, DMDRVI_AUDIO_FORMAT_S24_IN32_LE };
    DMOD_TEST_EXPECT_NOT_NULL(device(&f24));
    libdmaudio_stream_t s = open_stream(&MONO16_48K, 100);
    int16_t v[2] = { -2, 0x1234 };
    libdmaudio_write(s, v, 2, 0);
    int32_t out[2];
    libdmaudio_device_mix(g_dev, out, 2);
    DMOD_TEST_EXPECT_EQ(out[0], -2 * 256);
    DMOD_TEST_EXPECT_EQ(out[1], 0x1234 * 256);
    libdmaudio_close(s);

    libdmaudio_format_t f32 = { 48000, 1, DMDRVI_AUDIO_FORMAT_S32_LE };
    DMOD_TEST_EXPECT_NOT_NULL(device(&f32));
    s = open_stream(&f24, 100);
    int32_t in[1] = { -0x123456 };
    libdmaudio_write(s, in, 1, 0);
    libdmaudio_device_mix(g_dev, out, 1);
    DMOD_TEST_EXPECT_EQ(out[0], -0x12345600);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(a_full_buffer_takes_what_fits)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&MONO16_48K));
    libdmaudio_stream_options_t options = { .buffer_ms = 1, .volume_percent = 100 };   /* (64 frames at least) */
    libdmaudio_stream_t s = libdmaudio_open("test", &MONO16_48K, &options, NULL);
    DMOD_TEST_EXPECT_NOT_NULL(s);
    DMOD_TEST_EXPECT_EQ(libdmaudio_write(s, g_in, 100, 0), 64);
    DMOD_TEST_EXPECT_EQ(libdmaudio_write(s, g_in, 10, 0), -ETIMEDOUT);
    DMOD_TEST_EXPECT_EQ(libdmaudio_write(s, g_in, 10, 20), -ETIMEDOUT);
    libdmaudio_device_mix(g_dev, g_out, 10);
    DMOD_TEST_EXPECT_EQ(libdmaudio_write(s, g_in, 30, 0), 10);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(drain_waits_for_the_mixer)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&MONO16_48K));
    libdmaudio_stream_t s = open_stream(&MONO16_48K, 100);
    DMOD_TEST_EXPECT_EQ(libdmaudio_drain(s, 0), 0);
    libdmaudio_write(s, g_in, 20, 0);
    DMOD_TEST_EXPECT_EQ(libdmaudio_drain(s, 20), -ETIMEDOUT);
    libdmaudio_device_mix(g_dev, g_out, 20);
    DMOD_TEST_EXPECT_EQ(libdmaudio_drain(s, 20), 0);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(running_dry_is_an_underrun)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&MONO16_48K));
    libdmaudio_stream_t s = open_stream(&MONO16_48K, 100);
    libdmaudio_device_mix(g_dev, g_out, 10);           /* Nothing written yet: no underrun */
    libdmaudio_write(s, g_in, 10, 0);
    libdmaudio_device_mix(g_dev, g_out, 20);
    libdmaudio_stream_status_t st;
    libdmaudio_get_status(s, &st);
    DMOD_TEST_EXPECT_EQ(st.underruns, 1u);
    DMOD_TEST_EXPECT_EQ(st.buffered_frames, 0u);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(a_paused_stream_keeps_its_frames)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&MONO16_48K));
    libdmaudio_stream_t s = open_stream(&MONO16_48K, 100);
    g_in[0] = 777;
    libdmaudio_write(s, g_in, 1, 0);
    libdmaudio_set_paused(s, true);
    DMOD_TEST_EXPECT_EQ(libdmaudio_device_mix(g_dev, g_out, 1), 0);
    DMOD_TEST_EXPECT_EQ(g_out[0], 0);
    libdmaudio_set_paused(s, false);
    DMOD_TEST_EXPECT_EQ(libdmaudio_device_mix(g_dev, g_out, 1), 1);
    DMOD_TEST_EXPECT_EQ(g_out[0], 777);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(device_wait_sees_samples)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&MONO16_48K));
    libdmaudio_stream_t s = open_stream(&MONO16_48K, 100);
    DMOD_TEST_EXPECT_EQ(libdmaudio_device_wait(g_dev, 10), -ETIMEDOUT);
    libdmaudio_write(s, g_in, 5, 0);
    DMOD_TEST_EXPECT_EQ(libdmaudio_device_wait(g_dev, 10), 0);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(devices_by_name)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&STEREO16_48K));
    DMOD_TEST_EXPECT_NULL(libdmaudio_device_register("test", &MONO16_48K));     /* Taken */
    int status = 0;
    DMOD_TEST_EXPECT_NULL(libdmaudio_open("nothing", &MONO16_48K, NULL, &status));
    DMOD_TEST_EXPECT_EQ(status, -ENODEV);
    libdmaudio_format_t bad = { 48000, 9, DMDRVI_AUDIO_FORMAT_S16_LE };
    DMOD_TEST_EXPECT_NULL(libdmaudio_open("test", &bad, NULL, &status));
    DMOD_TEST_EXPECT_EQ(status, -EINVAL);

    char name[16];
    DMOD_TEST_EXPECT_EQ(libdmaudio_device_name(0, name, sizeof(name)), 0);
    DMOD_TEST_EXPECT_TRUE(strcmp(name, "test") == 0);
    DMOD_TEST_EXPECT_EQ(libdmaudio_device_name(1, name, sizeof(name)), -ENOENT);
    libdmaudio_stream_t s = libdmaudio_open(NULL, &MONO16_48K, NULL, &status);     /* The first device */
    DMOD_TEST_EXPECT_NOT_NULL(s);
    libdmaudio_device_info_t info;
    DMOD_TEST_EXPECT_EQ(libdmaudio_device_info(NULL, &info), 0);
    DMOD_TEST_EXPECT_EQ(info.streams, 1u);
    DMOD_TEST_EXPECT_EQ(info.format.channels, 2);
    DMOD_TEST_EXPECT_EQ(libdmaudio_device_set_volume("test", 40), 0);
    DMOD_TEST_EXPECT_EQ(libdmaudio_device_info("test", &info), 0);
    DMOD_TEST_EXPECT_EQ(info.volume_percent, 40);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(a_stream_outlives_its_device)
{
    DMOD_TEST_EXPECT_NOT_NULL(device(&MONO16_48K));
    libdmaudio_stream_t s = open_stream(&MONO16_48K, 100);
    libdmaudio_device_unregister(g_dev);
    g_dev = NULL;
    DMOD_TEST_EXPECT_EQ(libdmaudio_write(s, g_in, 4, 0), -ENODEV);
    libdmaudio_stream_status_t st;
    libdmaudio_get_status(s, &st);
    DMOD_TEST_EXPECT_TRUE(st.orphaned);
    libdmaudio_close(s);
}

DMOD_TEST_STEP(serve_options_from_arguments)
{
    libdmaudio_serve_options_t o;
    libdmaudio_serve_defaults(&o);
    DMOD_TEST_EXPECT_EQ(o.sample_rate_hz, 48000u);
    char* args[] = { "-r", "44100", "-v", "35", "-o", "speaker", "-p", "5" };
    DMOD_TEST_EXPECT_EQ(libdmaudio_serve_parse(8, args, &o), 0);
    DMOD_TEST_EXPECT_EQ(o.sample_rate_hz, 44100u);
    DMOD_TEST_EXPECT_EQ(o.volume_percent, 35);
    DMOD_TEST_EXPECT_EQ(o.output, DMDRVI_AUDIO_OUTPUT_SPEAKER);
    DMOD_TEST_EXPECT_EQ(o.period_ms, 5u);
    char* bad[] = { "-b", "20" };
    DMOD_TEST_EXPECT_EQ(libdmaudio_serve_parse(2, bad, &o), -EINVAL);
    char* missing[] = { "-r" };
    DMOD_TEST_EXPECT_EQ(libdmaudio_serve_parse(1, missing, &o), -EINVAL);
}
