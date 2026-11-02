/* The service is compiled into this native test so its imported node APIs
 * can be replaced without changing the imports of any running module. */
#include "../src/serve.c"
#include <stdio.h>
#include <stdlib.h>

#define PERIOD_BYTES 1920u
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; } } while (0)

static struct libdmaudio_device device;
dmosi_mutex_t g_lock;
static const size_t* writes;
static size_t write_count, write_index, accepted;
static unsigned starts, stops, drains, sleeps, mixes, closes, unregisters;
static unsigned start_failures;
static bool cancel_on_sleep, cancelled, finished, bad_data;
static int stop_error;

static void* open_node(const char* path, const char* mode)
{
    (void)path; (void)mode;
    return &device;
}

static int ioctl_node(void* node, int command, void* arg)
{
    (void)node;
    switch (command)
    {
        case DMDRVI_IOCTL_AUDIO_GET_STREAM:
        {
            dmdrvi_audio_stream_t* s = arg;
            memset(s, 0, sizeof(*s));
            s->sample_rate_hz = 48000;
            s->channels = 2;
            s->format = DMDRVI_AUDIO_FORMAT_S16_LE;
            s->transmit = true;
            break;
        }
        case DMDRVI_IOCTL_AUDIO_START:
            starts++;
            if (starts > 1 && start_failures > 0) { start_failures--; return -EIO; }
            break;
        case DMDRVI_IOCTL_AUDIO_STOP:
            stops++;
            if (starts > 0) return stop_error;
            break;
        case DMDRVI_IOCTL_AUDIO_DRAIN: drains++; break;
        default: break;
    }
    return 0;
}

static size_t write_node(const void* buffer, size_t size, size_t count, void* node)
{
    (void)node;
    if (size != 1 || write_index >= write_count) { bad_data = true; finished = true; return 0; }
    size_t result = writes[write_index++];
    if (result > count) return result;
    const uint8_t* data = buffer;
    for (size_t i = 0; i < result; i++)
        if (data[i] != (uint8_t)(accepted + i)) bad_data = true;
    accepted += result;
    if (accepted == PERIOD_BYTES) finished = true;
    return result;
}

static void close_node(void* node) { (void)node; closes++; }
static void* allocate(size_t size, const char* owner) { (void)owner; return malloc(size); }
static void release(void* pointer, bool concatenate) { (void)concatenate; free(pointer); }
static int lock_mutex(dmosi_mutex_t mutex) { (void)mutex; return 0; }
static int unlock_mutex(dmosi_mutex_t mutex) { (void)mutex; return 0; }
static bool logging(Dmod_LogLevel_t level) { (void)level; return false; }
static bool stop_requested(void) { return cancelled || finished; }
static void sleep_ms(uint32_t ms) { if (ms == 100u) { sleeps++; if (cancel_on_sleep) cancelled = true; } }

uint32_t format_bytes(dmdrvi_audio_format_t format) { (void)format; return 2; }
bool format_valid(const libdmaudio_format_t* format) { return format->channels == 2; }
uint32_t gain_of(uint8_t volume) { (void)volume; return GAIN_ONE; }
libdmaudio_device_t libdmaudio_device_register(const char* name, const libdmaudio_format_t* format)
{
    memset(&device, 0, sizeof(device));
    device.name = (char*)name;
    device.format = *format;
    device.frame_bytes = 4;
    return &device;
}
void libdmaudio_device_unregister(libdmaudio_device_t d) { (void)d; unregisters++; }
int libdmaudio_device_wait(libdmaudio_device_t d, int32_t timeout) { (void)d; (void)timeout; return 0; }
int libdmaudio_device_mix(libdmaudio_device_t d, void* buffer, uint32_t frames)
{
    (void)d; mixes++;
    for (size_t i = 0; i < frames * 4u; i++) ((uint8_t*)buffer)[i] = (uint8_t)i;
    return 1;
}

static int run_case(const size_t* script, size_t count)
{
    writes = script; write_count = count;
    write_index = accepted = 0;
    starts = stops = drains = sleeps = mixes = closes = unregisters = 0;
    cancelled = finished = bad_data = false;
    Dmod_FileOpen = open_node;
    Dmod_Ioctl = ioctl_node;
    Dmod_FileWrite = write_node;
    Dmod_FileClose = close_node;
    Dmod_MallocEx = allocate;
    Dmod_FreeEx = release;
    Dmod_CheckLogLevel = logging;
    dmosi_mutex_lock = lock_mutex;
    dmosi_mutex_unlock = unlock_mutex;
    dmosi_thread_sleep = sleep_ms;
    return libdmaudio_serve("test", "/test/audio", NULL, stop_requested);
}

static int test_healthy_short_writes(void)
{
    const size_t script[] = { 960, 960 };
    CHECK(run_case(script, 2) == 0);
    CHECK(!bad_data && mixes == 1 && accepted == PERIOD_BYTES);
    CHECK(starts == 1 && sleeps == 0 && drains == 1);
    return 0;
}

static int test_short_write_and_recovery(void)
{
    const size_t script[] = { 4, 0, PERIOD_BYTES - 4 };
    CHECK(run_case(script, 3) == 0);
    CHECK(!bad_data && mixes == 1 && accepted == PERIOD_BYTES);
    CHECK(starts == 2 && sleeps == 1 && drains == 1);
    CHECK(closes == 1 && unregisters == 1);
    return 0;
}

static int test_permanent_failure_is_bounded(void)
{
    const size_t script[] = { 0, 0, 0, 0 };
    CHECK(run_case(script, 4) == -EIO);
    CHECK(!bad_data && mixes == 1 && accepted == 0);
    CHECK(starts == 4 && sleeps == 3 && write_index == 4);
    CHECK(closes == 1 && unregisters == 1);
    return 0;
}

static int test_failed_start_can_recover(void)
{
    const size_t script[] = { 0, PERIOD_BYTES };
    start_failures = 1;
    CHECK(run_case(script, 2) == 0);
    CHECK(!bad_data && mixes == 1 && accepted == PERIOD_BYTES);
    CHECK(starts == 3 && sleeps == 2 && write_index == 2);
    return 0;
}

static int test_persistent_start_failure_is_bounded(void)
{
    const size_t script[] = { 0 };
    start_failures = 10;
    CHECK(run_case(script, 1) == -EIO);
    CHECK(!bad_data && mixes == 1 && accepted == 0);
    CHECK(starts == 4 && sleeps == 3 && write_index == 1);
    CHECK(closes == 1 && unregisters == 1);
    start_failures = 0;
    return 0;
}

static int test_cancel_during_recovery(void)
{
    const size_t script[] = { 0 };
    cancel_on_sleep = true;
    CHECK(run_case(script, 1) == 0);
    CHECK(mixes == 1 && starts == 1 && sleeps == 1 && drains == 0);
    CHECK(closes == 1 && unregisters == 1);
    cancel_on_sleep = false;
    return 0;
}

static int test_invalid_short_write(void)
{
    const size_t script[] = { 1 };
    CHECK(run_case(script, 1) == -EIO);
    CHECK(mixes == 1 && sleeps == 0);
    CHECK(closes == 1 && unregisters == 1);
    return 0;
}

static int test_failed_stop_is_reported(void)
{
    const size_t script[] = { 0 };
    stop_error = -ENODEV;
    CHECK(run_case(script, 1) == -ENODEV);
    CHECK(mixes == 1 && starts == 1 && sleeps == 0);
    CHECK(closes == 1 && unregisters == 1);
    stop_error = 0;
    return 0;
}

int main(void)
{
    int (*const cases[])(void) = {
        test_healthy_short_writes, test_short_write_and_recovery,
        test_permanent_failure_is_bounded, test_failed_start_can_recover,
        test_persistent_start_failure_is_bounded, test_cancel_during_recovery,
        test_invalid_short_write, test_failed_stop_is_reported,
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        start_failures = 0;
        stop_error = 0;
        cancel_on_sleep = false;
        failures += cases[i]();
    }
    printf("serve recovery: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures;
}
