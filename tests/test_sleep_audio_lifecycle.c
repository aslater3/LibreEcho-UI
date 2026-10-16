/*
 * Sleep/nursery source: real audiod lifecycle.
 *
 * This compiles the production audiod.c in and drives its real request
 * handler, so the fork/write/reap path, the bounded stop ramp and the status
 * document are exercised as shipped -- no re-implementation and no mock of
 * the sleep path.  The only thing the harness supplies is a media-bus file
 * where a device would have /run/libreecho-audio/media.pcm, and an
 * `output_available` flag the ALSA probe would otherwise set.
 *
 * Covered here: malformed and out-of-range arguments are rejected without
 * starting a writer; the legacy `colour` field still selects the source; the
 * status document exposes source/bed/tempo/fade alongside the legacy colour;
 * stop signals the child with SIGTERM so it ramps to silence (last frame is
 * zero, never a mid-sample kill) and reaps it (no zombie, no stale state);
 * and playback is refused when the output or the bus is absent.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

/* audiod.c honours this guard, so the test can point the media bus at a file
   inside its own build directory instead of the device runtime path. */
#ifndef LE_MEDIA_AUDIO_BUS
#define LE_MEDIA_AUDIO_BUS "/tmp/libreecho-sleep-lifecycle-bus.pcm"
#endif

#define main audiod_program_main
#include "../src/adapter/audiod.c"
#undef main

#include <stdio.h>
#include <sys/stat.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/*
 * Send one request through the real handler.  handle_request mutates the
 * message while parsing, so it gets a private copy.
 */
static int ipc(struct audio_hw *audio, const char *request, char *response,
               size_t size)
{
    char mutable[384];

    snprintf(mutable, sizeof(mutable), "%s", request);
    return handle_request(audio, mutable, response, size);
}

static void audio_fixture(struct audio_hw *audio)
{
    memset(audio, 0, sizeof(*audio));
    audio->ctl_fd = -1;
    /* The defaults audio_init would install, without opening a real card. */
    audio->noise_source = LE_SLEEP_SOURCE_WHITE;
    audio->noise_bed = LE_SLEEP_BED_NONE;
    audio->noise_tempo = LE_SLEEP_TEMPO_DEFAULT;
    /* The ALSA probe is what would set this on a device; the sleep path only
       reads it, so a host test supplies it directly. */
    audio->output_available = 1;
}

static int create_bus(void)
{
    int fd = open(LE_MEDIA_AUDIO_BUS, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd >= 0)
        close(fd);
    return fd >= 0 ? 0 : -1;
}

static long file_size(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

/* The status document must expose the sleep fields and the legacy alias. */
static int test_status_document(void)
{
    struct audio_hw audio;
    char response[1024];

    audio_fixture(&audio);
    CHECK(ipc(&audio, "{\"v\":1,\"id\":1,\"cmd\":\"status\"}", response,
              sizeof(response)) > 0);
    CHECK(strstr(response, "\"ok\":true") != NULL);
    CHECK(strstr(response, "\"noise_active\":false") != NULL);
    CHECK(strstr(response, "\"noise_source\":\"white\"") != NULL);
    CHECK(strstr(response, "\"noise_colour\":\"white\"") != NULL);
    CHECK(strstr(response, "\"noise_bed\":\"none\"") != NULL);
    CHECK(strstr(response, "\"noise_tempo\":60") != NULL);
    CHECK(strstr(response, "\"noise_fade_seconds\":0") != NULL);
    CHECK(strstr(response, "\"noise_level\":0") != NULL);
    CHECK(strstr(response, "\"noise_remaining_seconds\":-1") != NULL);
    return 0;
}

/*
 * Malformed input is rejected by the argument checks, before any writer is
 * forked, and the daemon stays honest about which field was wrong.
 */
static int test_malformed_arguments(void)
{
    struct audio_hw audio;
    char response[1024];
    static const struct {
        const char *request;
        const char *expect;
    } cases[] = {
        { "{\"v\":1,\"id\":2,\"cmd\":\"noise_start\",\"source\":\"green\"}",
          "source must be white, pink, brown or heartbeat" },
        { "{\"v\":1,\"id\":3,\"cmd\":\"noise_start\",\"source\":\"white\",\"level\":0}",
          "level must be 1-100 and minutes 0-600" },
        { "{\"v\":1,\"id\":4,\"cmd\":\"noise_start\",\"source\":\"white\",\"level\":101}",
          "level must be 1-100 and minutes 0-600" },
        { "{\"v\":1,\"id\":5,\"cmd\":\"noise_start\",\"source\":\"white\",\"minutes\":601}",
          "level must be 1-100 and minutes 0-600" },
        { "{\"v\":1,\"id\":6,\"cmd\":\"noise_start\",\"source\":\"white\",\"tempo\":39}",
          "tempo must be 40-100" },
        { "{\"v\":1,\"id\":7,\"cmd\":\"noise_start\",\"source\":\"white\",\"tempo\":101}",
          "tempo must be 40-100" },
        { "{\"v\":1,\"id\":8,\"cmd\":\"noise_start\",\"source\":\"white\",\"bed\":\"purple\"}",
          "bed must be none, pink or brown" },
        { "{\"v\":1,\"id\":9,\"cmd\":\"noise_start\",\"source\":\"white\",\"fade_seconds\":-1}",
          "fade_seconds must be 0-3600" },
        { "{\"v\":1,\"id\":10,\"cmd\":\"noise_start\",\"source\":\"white\",\"fade_seconds\":3601}",
          "fade_seconds must be 0-3600" },
        { "{\"v\":1,\"id\":11,\"cmd\":\"noise_start\",\"source\":\"white\",\"bed\":\"purple\",\"tempo\":1000}",
          "bed must be none, pink or brown" }
    };
    size_t i;

    audio_fixture(&audio);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CHECK(ipc(&audio, cases[i].request, response, sizeof(response)) > 0);
        CHECK(strstr(response, "\"ok\":false") != NULL);
        CHECK(strstr(response, cases[i].expect) != NULL);
        /* No writer was ever started. */
        CHECK(audio.noise_pid == 0);
    }
    return 0;
}

/* Playback is refused when the output or the bus is not there. */
static int test_refused_without_output_or_bus(void)
{
    struct audio_hw audio;
    char response[1024];

    /* Output unavailable. */
    audio_fixture(&audio);
    CHECK(create_bus() == 0);
    audio.output_available = 0;
    CHECK(ipc(&audio, "{\"v\":1,\"id\":12,\"cmd\":\"noise_start\",\"source\":\"pink\",\"level\":40}",
              response, sizeof(response)) > 0);
    CHECK(strstr(response, "\"ok\":false") != NULL);
    CHECK(strstr(response, "audio output unavailable") != NULL);
    CHECK(audio.noise_pid == 0);

    /* Output available, but the media bus file is gone. */
    audio_fixture(&audio);
    CHECK(unlink(LE_MEDIA_AUDIO_BUS) == 0 || errno == ENOENT);
    CHECK(ipc(&audio, "{\"v\":1,\"id\":13,\"cmd\":\"noise_start\",\"source\":\"pink\",\"level\":40}",
              response, sizeof(response)) > 0);
    CHECK(strstr(response, "\"ok\":false") != NULL);
    CHECK(strstr(response, "audio output unavailable") != NULL);
    CHECK(audio.noise_pid == 0);
    return 0;
}

/* The pre-heartbeat `colour` field still selects the source. */
static int test_legacy_colour(void)
{
    struct audio_hw audio;
    char response[1024];

    audio_fixture(&audio);
    CHECK(create_bus() == 0);
    CHECK(ipc(&audio, "{\"v\":1,\"id\":14,\"cmd\":\"noise_start\",\"colour\":\"brown\",\"level\":40,\"minutes\":1}",
              response, sizeof(response)) > 0);
    CHECK(strstr(response, "\"ok\":true") != NULL);
    CHECK(audio.noise_pid > 0);
    CHECK(ipc(&audio, "{\"v\":1,\"id\":15,\"cmd\":\"status\"}", response,
              sizeof(response)) > 0);
    CHECK(strstr(response, "\"noise_source\":\"brown\"") != NULL);
    CHECK(strstr(response, "\"noise_colour\":\"brown\"") != NULL);
    CHECK(strstr(response, "\"noise_active\":true") != NULL);
    CHECK(ipc(&audio, "{\"v\":1,\"id\":16,\"cmd\":\"noise_stop\"}", response,
              sizeof(response)) > 0);
    CHECK(strstr(response, "\"ok\":true") != NULL);
    CHECK(audio.noise_pid == 0);
    return 0;
}

/*
 * The heart of the lifecycle: a running writer is stopped with SIGTERM, must
 * ramp to silence (not be killed mid-sample), must be reaped, and must not
 * keep the bus open afterwards.
 */
static int test_lifecycle_fade_and_cleanup(void)
{
    struct audio_hw audio;
    char response[1024];
    unsigned char tail[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
    pid_t writer;
    long size_before, size_after;
    int fd, status;

    audio_fixture(&audio);
    CHECK(create_bus() == 0);
    CHECK(ipc(&audio, "{\"v\":1,\"id\":17,\"cmd\":\"noise_start\","
                      "\"source\":\"heartbeat\",\"bed\":\"brown\",\"tempo\":50,"
                      "\"level\":60,\"minutes\":1,\"fade_seconds\":10}",
              response, sizeof(response)) > 0);
    CHECK(strstr(response, "\"ok\":true") != NULL);
    writer = audio.noise_pid;
    CHECK(writer > 0);

    /* The status reflects the requested configuration. */
    CHECK(ipc(&audio, "{\"v\":1,\"id\":18,\"cmd\":\"status\"}", response,
              sizeof(response)) > 0);
    CHECK(strstr(response, "\"noise_source\":\"heartbeat\"") != NULL);
    CHECK(strstr(response, "\"noise_bed\":\"brown\"") != NULL);
    CHECK(strstr(response, "\"noise_tempo\":50") != NULL);
    CHECK(strstr(response, "\"noise_fade_seconds\":10") != NULL);
    CHECK(strstr(response, "\"noise_level\":60") != NULL);
    CHECK(strstr(response, "\"noise_active\":true") != NULL);

    /* Let the writer actually produce bus audio. */
    usleep(60000);
    size_before = file_size(LE_MEDIA_AUDIO_BUS);
    CHECK(size_before > 0);

    CHECK(ipc(&audio, "{\"v\":1,\"id\":19,\"cmd\":\"noise_stop\"}", response,
              sizeof(response)) > 0);
    CHECK(strstr(response, "\"ok\":true") != NULL);
    CHECK(audio.noise_pid == 0);
    /* The run counters are cleared; the last selection is retained so status
       can still describe what was playing. */
    CHECK(audio.noise_seconds == 0 && audio.noise_level == 0 &&
          audio.noise_fade_seconds == 0);
    CHECK(audio.noise_bed == LE_SLEEP_BED_BROWN);

    /* Reaped: the child is gone, so it cannot be waited on again. */
    CHECK(waitpid(writer, &status, WNOHANG) < 0 && errno == ECHILD);

    /* The writer stopped: the bus file has not grown since the stop. */
    size_after = file_size(LE_MEDIA_AUDIO_BUS);
    usleep(50000);
    CHECK(file_size(LE_MEDIA_AUDIO_BUS) == size_after);
    CHECK(size_after >= size_before);

    /* The final frame is exactly silence: a bounded ramp, not a cut. */
    CHECK(size_after >= 4);
    fd = open(LE_MEDIA_AUDIO_BUS, O_RDONLY);
    CHECK(fd >= 0);
    CHECK(lseek(fd, size_after - 4, SEEK_SET) == size_after - 4);
    CHECK(read(fd, tail, sizeof(tail)) == (ssize_t)sizeof(tail));
    close(fd);
    CHECK(tail[0] == 0 && tail[1] == 0 && tail[2] == 0 && tail[3] == 0);

    /* A second stop is a no-op, not an error. */
    CHECK(ipc(&audio, "{\"v\":1,\"id\":20,\"cmd\":\"noise_stop\"}", response,
              sizeof(response)) > 0);
    CHECK(strstr(response, "\"ok\":true") != NULL);
    return 0;
}

int main(void)
{
    (void)unlink(LE_MEDIA_AUDIO_BUS);

    CHECK(test_status_document() == 0);
    CHECK(test_malformed_arguments() == 0);
    CHECK(test_refused_without_output_or_bus() == 0);
    CHECK(test_legacy_colour() == 0);
    CHECK(test_lifecycle_fade_and_cleanup() == 0);

    (void)unlink(LE_MEDIA_AUDIO_BUS);
    puts("audiod sleep source lifecycle: args, refusal, legacy colour, "
         "status, fade stop and writer cleanup: ok");
    return 0;
}
