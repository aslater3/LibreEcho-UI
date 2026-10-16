#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

/*
 * Cue feedback must not become a rapid-fire burst.
 *
 * Every cue request used to fork its own writer onto the shared system PCM,
 * so N triggers in a window produced N overlapping short playbacks -- the
 * stream-lock contention shape that panics the AFE period IRQ path.  These
 * checks drive the real request handler against a local FIFO (never ALSA),
 * so playback is counted from the bytes the daemon actually wrote: one
 * chirp's worth of PCM means exactly one writer ran.
 */
#ifndef LE_AUDIOD_SOURCE
#define LE_AUDIOD_SOURCE "../src/adapter/audiod.c"
#endif
#define main audiod_program_main
#include LE_AUDIOD_SOURCE
#undef main

#include <stdio.h>

#define CHECK(condition) do {                                               \
    if (!(condition)) {                                                     \
        fprintf(stderr, "cue rate limit: check failed at %s:%d: %s\n",      \
                __FILE__, __LINE__, #condition);                            \
        return 1;                                                           \
    }                                                                       \
} while (0)

/* Stereo int16 at the bus rate: what write_chirp_fd emits for a duration. */
#define CHIRP_BYTES(ms) ((size_t)LE_PCM_RATE * (size_t)(ms) / 1000U *       \
                         LE_TONE_CHANNELS * sizeof(int16_t))

/* Longer than any plausible minimum interval, so the next request is
   outside the window; short enough to keep the test quick. */
#define SETTLE_MS 350
/* A drain stops once the bus has been quiet this long. */
#define QUIET_MS 400
#define BURST 8

static int fifo_fd = -1;
static size_t bus_bytes;

static long long test_millis(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/*
 * Read until the bus has been quiet for quiet_ms.  A closed write end is
 * EOF, not silence: the daemon forks a writer per cue, so the read end sees
 * EOF between cues and that must not end the drain early.
 */
static size_t drain_bus(int quiet_ms)
{
    unsigned char buffer[8192];
    long long deadline = test_millis() + quiet_ms;
    size_t got = 0;

    for (;;) {
        struct pollfd ready = { fifo_fd, POLLIN, 0 };
        ssize_t count;

        if (test_millis() >= deadline)
            break;
        if (poll(&ready, 1, 20) <= 0)
            continue;
        count = read(fifo_fd, buffer, sizeof(buffer));
        if (count > 0) {
            got += (size_t)count;
            deadline = test_millis() + quiet_ms;
        } else if (count == 0) {
            usleep(20 * 1000);
        } else {
            break;
        }
    }
    bus_bytes += got;
    return got;
}

static size_t read_bus(size_t limit, int wait_ms)
{
    unsigned char buffer[1024];
    struct pollfd ready = { fifo_fd, POLLIN, 0 };
    ssize_t count;

    if (poll(&ready, 1, wait_ms) <= 0)
        return 0;
    if (limit > sizeof(buffer))
        limit = sizeof(buffer);
    count = read(fifo_fd, buffer, limit);
    if (count <= 0)
        return 0;
    bus_bytes += (size_t)count;
    return (size_t)count;
}

/*
 * Drive one request through the daemon's own handler.  Returns 1 when the
 * daemon accepted the request as a playback, 0 when it accepted it without
 * playing (coalesced/dropped), -1 when it rejected the request outright.
 */
static int send_request(struct audio_hw *audio, const char *command,
                        const char *args, char *response,
                        size_t response_size)
{
    static unsigned long id;
    char request[320];

    ++id;
    if (args)
        snprintf(request, sizeof(request),
                 "{\"v\":1,\"id\":%lu,\"cmd\":\"%s\",\"args\":%s}",
                 id, command, args);
    else
        snprintf(request, sizeof(request),
                 "{\"v\":1,\"id\":%lu,\"cmd\":\"%s\"}", id, command);
    handle_request(audio, request, response, response_size);
    if (!strstr(response, "\"ok\":true"))
        return -1;
    return strstr(response, "\"playing\":false") ? 0 : 1;
}

static void settle(void)
{
    usleep(SETTLE_MS * 1000);
}

int main(void)
{
    char directory[] = "/tmp/libreecho-cue-rate-limit-XXXXXX";
    char fifo_path[256], response[1024];
    struct audio_hw audio;
    int played, accepted, throttled, before;
    int i;

    if (!mkdtemp(directory))
        return 1;
    snprintf(fifo_path, sizeof(fifo_path), "%s/system.pcm", directory);
    if (mkfifo(fifo_path, 0600) < 0) {
        rmdir(directory);
        return 1;
    }
    fifo_fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
    if (fifo_fd < 0) {
        unlink(fifo_path);
        rmdir(directory);
        return 1;
    }
    memset(&audio, 0, sizeof(audio));
    audio.output_available = 1;
    snprintf(audio.system_audio_bus, sizeof(audio.system_audio_bus),
             "%s", fifo_path);

    /* 1. A burst of cue requests inside the window: one playback, one
       writer, and no error responses to retry on. */
    before = (int)bus_bytes;
    played = 0;
    accepted = 0;
    throttled = 0;
    for (i = 0; i < BURST; ++i) {
        int result = send_request(
            &audio, "cue",
            "{\"first_hz\":660,\"second_hz\":990,\"ms\":90}",
            response, sizeof(response));

        if (result < 0) {
            fprintf(stderr, "cue rate limit: burst request %d rejected: %s\n",
                    i, response);
            goto done;
        }
        accepted += result;
        played += result;
        if (strstr(response, "\"throttled\":true"))
            ++throttled;
    }
    drain_bus(QUIET_MS);
    if (played != 1 || throttled != BURST - 1 ||
        bus_bytes - (size_t)before != CHIRP_BYTES(90))
        fprintf(stderr,
                "cue rate limit: burst played=%d throttled=%d bytes=%zu "
                "(one chirp is %zu)\n",
                played, throttled, bus_bytes - (size_t)before,
                CHIRP_BYTES(90));
    CHECK(played == 1);
    CHECK(accepted == 1);
    CHECK(throttled == BURST - 1);
    CHECK(bus_bytes - (size_t)before == CHIRP_BYTES(90));

    /*
     * 2. The dropped requests must not be queued: nothing else may play
     * once the window has passed without a new request.
     */
    usleep(600 * 1000);
    CHECK(drain_bus(QUIET_MS) == 0);

    /*
     * 3. A request inside the minimum interval, after the previous writer
     *    has already finished, is still dropped: the limit is an interval,
     *    not just an in-flight guard.  The short drain and the 60ms sleep
     *    put the follow-up roughly 120ms after the cue, inside a 200ms
     *    window; usleep only ever oversleeps, so this only slips past the
     *    window if the runner stalls for tens of milliseconds mid-sleep.
     */
    settle();
    CHECK(send_request(&audio, "cue",
                       "{\"first_hz\":660,\"second_hz\":990,\"ms\":90}",
                       response, sizeof(response)) == 1);
    drain_bus(50);
    usleep(60 * 1000);
    before = (int)bus_bytes;
    played = 0;
    for (i = 0; i < 3; ++i) {
        int result = send_request(
            &audio, "cue",
            "{\"first_hz\":660,\"second_hz\":990,\"ms\":90}",
            response, sizeof(response));

        if (result < 0) {
            fprintf(stderr, "cue rate limit: in-window request %d rejected: %s\n",
                    i, response);
            goto done;
        }
        played += result;
    }
    drain_bus(QUIET_MS);
    CHECK(played == 0);
    CHECK(bus_bytes - (size_t)before == 0);

    /*
     * 4. A cue that arrives while the previous one is still playing is
     *    dropped rather than layered on top of it.  The reader is paced --
     *    one 1 KiB read every 20ms -- so the 800ms chirp's writer only
     *    finishes once its whole chirp has been read: whatever the bus
     *    buffer size, at 300ms that writer is still live and the minimum
     *    interval has already expired, so only the in-flight guard can
     *    refuse the request.
     */
    settle();
    CHECK(send_request(&audio, "cue",
                       "{\"first_hz\":660,\"second_hz\":990,\"ms\":800}",
                       response, sizeof(response)) == 1);
    before = (int)bus_bytes;
    played = 0;
    for (i = 0; i < 15; ++i) {
        usleep(20 * 1000);
        read_bus(1024, 0);
    }
    for (i = 0; i < 3; ++i) {
        int result = send_request(
            &audio, "cue",
            "{\"first_hz\":660,\"second_hz\":990,\"ms\":90}",
            response, sizeof(response));

        if (result < 0) {
            fprintf(stderr, "cue rate limit: in-flight request %d rejected: %s\n",
                    i, response);
            goto done;
        }
        played += result;
    }
    drain_bus(QUIET_MS);
    CHECK(played == 0);
    CHECK(bus_bytes - (size_t)before == CHIRP_BYTES(800));

    /* 5. Outside the window a request plays normally again. */
    settle();
    before = (int)bus_bytes;
    CHECK(send_request(&audio, "cue",
                       "{\"first_hz\":660,\"second_hz\":990,\"ms\":90}",
                       response, sizeof(response)) == 1);
    drain_bus(QUIET_MS);
    CHECK(bus_bytes - (size_t)before == CHIRP_BYTES(90));

    /* 6. wake_chirp goes through the same boundary. */
    settle();
    before = (int)bus_bytes;
    played = 0;
    for (i = 0; i < BURST; ++i) {
        int result = send_request(&audio, "wake_chirp", NULL, response,
                                  sizeof(response));

        if (result < 0) {
            fprintf(stderr, "cue rate limit: wake_chirp %d rejected: %s\n",
                    i, response);
            goto done;
        }
        played += result;
    }
    drain_bus(QUIET_MS);
    if (played != 1 || bus_bytes - (size_t)before != CHIRP_BYTES(LE_CHIRP_MS))
        fprintf(stderr,
                "cue rate limit: wake_chirp burst played=%d bytes=%zu "
                "(one chirp is %zu)\n",
                played, bus_bytes - (size_t)before,
                CHIRP_BYTES(LE_CHIRP_MS));
    CHECK(played == 1);

    /* 7. The existing duration cap and frequency validation are untouched:
       out-of-range durations fall back to the 90 ms chirp, and out-of-range
       frequencies are still rejected. */
    settle();
    before = (int)bus_bytes;
    CHECK(send_request(&audio, "cue",
                       "{\"first_hz\":660,\"second_hz\":990,\"ms\":5000}",
                       response, sizeof(response)) == 1);
    drain_bus(QUIET_MS);
    CHECK(bus_bytes - (size_t)before == CHIRP_BYTES(LE_CHIRP_MS));

    settle();
    before = (int)bus_bytes;
    CHECK(send_request(&audio, "cue",
                       "{\"first_hz\":660,\"second_hz\":990,\"ms\":39}",
                       response, sizeof(response)) == 1);
    drain_bus(QUIET_MS);
    CHECK(bus_bytes - (size_t)before == CHIRP_BYTES(LE_CHIRP_MS));

    settle();
    CHECK(send_request(&audio, "cue",
                       "{\"first_hz\":10,\"second_hz\":990,\"ms\":90}",
                       response, sizeof(response)) == -1);
    CHECK(strstr(response, "\"ok\":false") != NULL);

    settle();
    before = (int)bus_bytes;
    CHECK(send_request(&audio, "cue",
                       "{\"first_hz\":60,\"second_hz\":8000,\"ms\":40}",
                       response, sizeof(response)) == 1);
    drain_bus(QUIET_MS);
    CHECK(bus_bytes - (size_t)before == CHIRP_BYTES(40));

    puts("cue rate limit: burst coalescing, bounded writers, and "
         "interval recovery: ok");
    close(fifo_fd);
    unlink(fifo_path);
    rmdir(directory);
    return 0;
done:
    close(fifo_fd);
    unlink(fifo_path);
    rmdir(directory);
    return 1;
}
