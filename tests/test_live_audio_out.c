#define _POSIX_C_SOURCE 200809L

/*
 * Model audio into the playback bus.
 *
 * Four properties matter and none of them is about audio quality: the write
 * must land in the bus format, a bus that is missing must fail loudly rather
 * than hang, a bus that stops draining must not hold the daemon's poll loop -
 * and with it the wake path - hostage, and a rate the fixed staging buffer
 * cannot carry must be refused rather than silently resampled wrong.
 */

#include "adapter/live_audio_out.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static char directory[64];

static int join_path(char *out, size_t size, const char *name)
{
    size_t base = strlen(directory);
    size_t extra = strlen(name);

    if (base + 1 + extra + 1 > size)
        return -1;
    memcpy(out, directory, base);
    out[base] = '/';
    memcpy(out + base + 1, name, extra);
    out[base + 1 + extra] = '\0';
    return 0;
}

static int make_bus(const char *name, char *path, size_t size)
{
    if (join_path(path, size, name) < 0)
        return -1;
    return close(open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600));
}

static int write_engine_status(int system_active)
{
    char path[160];
    const char *text = system_active
        ? "{\"buses\":{\"system\":false},\"drain\":{\"system\":{\"drained\":false}}}\n"
        : "{\"buses\":{\"system\":false},\"drain\":{\"system\":{\"drained\":true}}}\n";
    int fd;

    if (join_path(path, sizeof(path), "status.json") < 0)
        return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    if (write(fd, text, strlen(text)) != (ssize_t)strlen(text)) {
        close(fd);
        return -1;
    }
    return close(fd);
}

static void fill(int16_t *samples, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i)
        samples[i] = (int16_t)((int)(i % 32) < 16 ? 5000 : -5000);
}

static int test_regular_file_receives_speaker_format(void)
{
    struct le_live_audio_out out;
    char path[160];
    int16_t samples[LE_LIVE_AUDIO_SAMPLES];
    struct stat status;

    CHECK(make_bus("bus.pcm", path, sizeof(path)) == 0);
    le_live_audio_out_init(&out, path, 48000);
    fill(samples, LE_LIVE_AUDIO_SAMPLES);
    /* 1280 mono frames at 24 kHz are 2560 stereo frames at 48 kHz, less the
       resampler's two-frame lookahead. */
    CHECK(le_live_audio_out_write(&out, samples, LE_LIVE_AUDIO_SAMPLES,
                                  24000) == 0);
    CHECK(out.frames_written >= 2550);
    CHECK(out.frames_written <= 2560);
    CHECK(out.write_errors == 0);
    /* The bus carries interleaved stereo, so the file is frames * 2 * 2. */
    CHECK(stat(path, &status) == 0);
    CHECK(status.st_size == (off_t)(out.frames_written * 4));
    le_live_audio_out_close(&out);
    unlink(path);
    return 0;
}

static int test_input_rate_is_honoured(void)
{
    struct le_live_audio_out out;
    char path[160];
    int16_t samples[LE_LIVE_AUDIO_SAMPLES];
    uint64_t at_48k;
    uint64_t at_16k;

    CHECK(make_bus("rate.pcm", path, sizeof(path)) == 0);
    fill(samples, LE_LIVE_AUDIO_SAMPLES);

    /* A 16 kHz source triples; it must not be treated as a pass-through. */
    le_live_audio_out_init(&out, path, 48000);
    CHECK(le_live_audio_out_write(&out, samples, LE_LIVE_AUDIO_SAMPLES,
                                  16000) == 0);
    at_16k = out.frames_written;
    CHECK(at_16k >= 3830);
    CHECK(at_16k <= 3840);
    le_live_audio_out_close(&out);

    le_live_audio_out_init(&out, path, 48000);
    CHECK(le_live_audio_out_write(&out, samples, LE_LIVE_AUDIO_SAMPLES,
                                  48000) == 0);
    at_48k = out.frames_written;
    CHECK(at_48k >= 1270);
    CHECK(at_48k <= 1280);
    le_live_audio_out_close(&out);

    unlink(path);
    return 0;
}

static int test_missing_bus_fails_loudly(void)
{
    struct le_live_audio_out out;
    int16_t samples[LE_LIVE_AUDIO_SAMPLES];

    le_live_audio_out_init(&out, "/nonexistent/directory/system.pcm", 48000);
    fill(samples, LE_LIVE_AUDIO_SAMPLES);
    CHECK(le_live_audio_out_write(&out, samples, LE_LIVE_AUDIO_SAMPLES,
                                  16000) == -1);
    CHECK(out.write_errors >= 1);
    CHECK(out.frames_written == 0);
    le_live_audio_out_close(&out);
    return 0;
}

static int test_unusable_rate_is_refused_not_approximated(void)
{
    struct le_live_audio_out out;
    char path[160];
    int16_t samples[LE_LIVE_AUDIO_SAMPLES];
    struct stat status;

    CHECK(make_bus("narrow.pcm", path, sizeof(path)) == 0);
    le_live_audio_out_init(&out, path, 48000);
    fill(samples, LE_LIVE_AUDIO_SAMPLES);
    /* Below the lowest rate whose output fits the staging buffer. */
    CHECK(le_live_audio_out_write(&out, samples, LE_LIVE_AUDIO_SAMPLES,
                                  2000) == -1);
    CHECK(out.frames_written == 0);
    /* Refused rather than truncated: the bus must stay silent, not short. */
    CHECK(stat(path, &status) == 0);
    CHECK(status.st_size == 0);
    /* The floor itself is usable. */
    CHECK(le_live_audio_out_write(&out, samples, LE_LIVE_AUDIO_SAMPLES,
                                  8000) == 0);
    CHECK(out.frames_written > 0);
    le_live_audio_out_close(&out);
    unlink(path);
    return 0;
}

static int test_unsupported_speaker_rate_is_refused(void)
{
    struct le_live_audio_out out;
    char path[160];
    int16_t samples[LE_LIVE_AUDIO_SAMPLES];

    CHECK(make_bus("wrongbus.pcm", path, sizeof(path)) == 0);
    /*
     * The shared resampler targets the platform bus rate.  A device configured
     * for a different one gets silence with an error, not audio at the wrong
     * pitch.
     */
    le_live_audio_out_init(&out, path, 44100);
    fill(samples, LE_LIVE_AUDIO_SAMPLES);
    CHECK(le_live_audio_out_write(&out, samples, LE_LIVE_AUDIO_SAMPLES,
                                  16000) == -1);
    CHECK(out.frames_written == 0);
    le_live_audio_out_close(&out);
    unlink(path);
    return 0;
}

static int test_stalled_bus_gives_up_within_the_budget(void)
{
    struct le_live_audio_out out;
    char path[160];
    int16_t samples[LE_LIVE_AUDIO_SAMPLES];
    struct timespec start;
    struct timespec finish;
    long elapsed_ms;
    int reader;
    int result = 0;
    int i;

    CHECK(join_path(path, sizeof(path), "stalled.fifo") == 0);
    unlink(path);
    CHECK(mkfifo(path, 0600) == 0);
    /* A reader that never reads: the pipe buffer fills and the writer stalls. */
    reader = open(path, O_RDONLY | O_NONBLOCK);
    CHECK(reader >= 0);
    le_live_audio_out_init(&out, path, 48000);
    fill(samples, LE_LIVE_AUDIO_SAMPLES);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (i = 0; i < 64 && result == 0; ++i)
        result = le_live_audio_out_write(&out, samples,
                                         LE_LIVE_AUDIO_SAMPLES, 16000);
    clock_gettime(CLOCK_MONOTONIC, &finish);
    elapsed_ms = (long)(finish.tv_sec - start.tv_sec) * 1000L +
                 (finish.tv_nsec - start.tv_nsec) / 1000000L;

    /* The write must have failed, and the failure must be bounded. */
    CHECK(result == -1);
    CHECK(out.stall_timeouts >= 1);
    CHECK(elapsed_ms < 15000);
    close(reader);
    le_live_audio_out_close(&out);
    unlink(path);
    return 0;
}

static int test_cancel_stops_playback_and_closes_the_turn(void)
{
    struct le_live_audio_out out;
    char path[160];
    int16_t samples[LE_LIVE_AUDIO_SAMPLES];
    uint64_t after_first;

    CHECK(make_bus("cancel.pcm", path, sizeof(path)) == 0);
    le_live_audio_out_init(&out, path, 48000);
    fill(samples, LE_LIVE_AUDIO_SAMPLES);
    CHECK(le_live_audio_out_write(&out, samples, LE_LIVE_AUDIO_SAMPLES,
                                  16000) == 0);
    after_first = out.frames_written;
    CHECK(out.turn_open == 1);

    le_live_audio_out_cancel(&out);
    CHECK(out.cancelled == 1);
    CHECK(out.cancels == 1);
    /* A barge-in ends the turn, so the next chunk starts a fresh resampler
       phase rather than splicing the cancelled tail into the new sentence. */
    CHECK(out.turn_open == 0);

    CHECK(le_live_audio_out_write(&out, samples, LE_LIVE_AUDIO_SAMPLES,
                                  16000) == 0);
    CHECK(out.cancelled == 0);
    CHECK(out.turn_open == 1);
    /* The second turn produced the same amount as the first, from a reset
       resampler: no phase carry-over. */
    CHECK(out.frames_written - after_first == after_first);
    le_live_audio_out_close(&out);
    unlink(path);
    return 0;
}

static int test_rejects_a_chunk_larger_than_the_transport_contract(void)
{
    struct le_live_audio_out out;
    char path[160];
    int16_t samples[LE_LIVE_AUDIO_SAMPLES + 1];

    CHECK(make_bus("oversize.pcm", path, sizeof(path)) == 0);
    le_live_audio_out_init(&out, path, 48000);
    fill(samples, LE_LIVE_AUDIO_SAMPLES + 1);
    CHECK(le_live_audio_out_write(&out, samples, LE_LIVE_AUDIO_SAMPLES + 1,
                                  16000) == -1);
    CHECK(out.frames_written == 0);
    le_live_audio_out_close(&out);
    unlink(path);
    return 0;
}

static int test_drain_follows_audio_engine_status(void)
{
    struct le_live_audio_out out;
    char path[160];

    CHECK(make_bus("drain.pcm", path, sizeof(path)) == 0);
    le_live_audio_out_init(&out, path, 48000);
    CHECK(write_engine_status(1) == 0);
    CHECK(le_live_audio_out_drained(&out) == 0);
    CHECK(write_engine_status(0) == 0);
    CHECK(le_live_audio_out_drained(&out) == 1);
    CHECK(out.drain_checks == 2);
    CHECK(out.drain_confirmations == 1);
    le_live_audio_out_close(&out);
    unlink(path);
    CHECK(join_path(path, sizeof(path), "status.json") == 0);
    unlink(path);
    return 0;
}

static int test_cancel_notifies_system_bus_only(void)
{
    struct le_live_audio_out out;
    struct sockaddr_un address;
    char bus[160], control[160], message[64];
    int fd;
    ssize_t count;

    CHECK(make_bus("system.pcm", bus, sizeof(bus)) == 0);
    CHECK(join_path(control, sizeof(control), "control.sock") == 0);
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    CHECK(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    CHECK(strlen(control) < sizeof(address.sun_path));
    strcpy(address.sun_path, control);
    unlink(control);
    CHECK(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    le_live_audio_out_init(&out, bus, 48000);
    le_live_audio_out_cancel(&out);
    count = recv(fd, message, sizeof(message) - 1U, 0);
    CHECK(count > 0);
    message[count] = '\0';
    CHECK(!strcmp(message, "cancel system"));
    CHECK(out.cancel_requests == 1);
    CHECK(out.cancel_failures == 0);
    le_live_audio_out_close(&out);
    close(fd);
    unlink(control);
    unlink(bus);
    return 0;
}

int main(void)
{
    int failures = 0;

    snprintf(directory, sizeof(directory), "/tmp/le-live-out-%d",
             (int)getpid());
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create %s\n", directory);
        return 1;
    }
    failures += test_regular_file_receives_speaker_format() != 0;
    failures += test_input_rate_is_honoured() != 0;
    failures += test_missing_bus_fails_loudly() != 0;
    failures += test_unusable_rate_is_refused_not_approximated() != 0;
    failures += test_unsupported_speaker_rate_is_refused() != 0;
    failures += test_stalled_bus_gives_up_within_the_budget() != 0;
    failures += test_cancel_stops_playback_and_closes_the_turn() != 0;
    failures += test_rejects_a_chunk_larger_than_the_transport_contract() != 0;
    failures += test_drain_follows_audio_engine_status() != 0;
    failures += test_cancel_notifies_system_bus_only() != 0;
    rmdir(directory);
    if (failures) {
        fprintf(stderr, "live audio out: FAILED\n");
        return 1;
    }
    printf("live audio out: ok\n");
    return 0;
}
