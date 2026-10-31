#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

/* Owner event sounds (#410) in audiod: real playback through a local FIFO,
   never ALSA. */
#ifndef LE_AUDIOD_SOURCE
#define LE_AUDIOD_SOURCE "../src/adapter/audiod.c"
#endif
#define main audiod_program_main
#include LE_AUDIOD_SOURCE
#undef main

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed at %s:%d: %s (%s)\n", __FILE__, __LINE__, #c, response); goto done; } } while (0)

static char response[1024];

static void call(struct audio_hw *audio, const char *cmd, const char *args)
{
    char request[256];

    snprintf(request, sizeof(request), "{\"v\":1,\"id\":1,\"cmd\":\"%s\",\"args\":%s}", cmd, args);
    response[0] = '\0';
    handle_request(audio, request, response, sizeof(response));
}

/* Read everything the child writes; returns bytes, or -1 on a stall. */
static long drain(int fd, int16_t *first, size_t first_count)
{
    unsigned char buffer[4096];
    long total = 0;

    for (;;) {
        struct pollfd ready = { fd, POLLIN, 0 };
        ssize_t count;

        if (poll(&ready, 1, 2000) <= 0)
            return -1;
        count = read(fd, buffer, sizeof(buffer));
        if (count == 0)
            return total;
        if (count < 0)
            return -1;
        if (first && (size_t)total < first_count * 2) {
            size_t copy = (size_t)count;
            if (copy > first_count * 2 - (size_t)total)
                copy = first_count * 2 - (size_t)total;
            memcpy((unsigned char *)first + total, buffer, copy);
        }
        total += count;
    }
}

static int write_raw(const char *path, size_t samples, int16_t value)
{
    static int16_t data[LE_EVENT_SOUND_MAX_SAMPLES + 2];
    FILE *f = fopen(path, "wb");
    size_t i;
    int ok;

    if (!f)
        return -1;
    for (i = 0; i < samples; ++i)
        data[i] = value;
    ok = fwrite(data, sizeof(int16_t), samples, f) == samples;
    return fclose(f) == 0 && ok ? 0 : -1;
}

static int reap(pid_t child)
{
    int status;

    return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0 ? 0 : -1;
}

int main(void)
{
    char directory[] = "/tmp/libreecho-event-sound-XXXXXX";
    char path[256], saved[256], fifo[256];
    struct audio_hw audio;
    int16_t first[4];
    int fifo_fd = -1, status = 1;
    pid_t child = 0;

    if (!mkdtemp(directory) || setenv(LE_EVENT_SOUND_DIR_ENV, directory, 1))
        return 1;
    snprintf(path, sizeof(path), "%s/sound-timer.raw", directory);
    snprintf(saved, sizeof(saved), "%s/sound-wake.raw.saved", directory);
    snprintf(fifo, sizeof(fifo), "%s/system.fifo", directory);
    if (mkfifo(fifo, 0600) < 0 || (fifo_fd = open(fifo, O_RDONLY | O_NONBLOCK)) < 0)
        goto done;
    memset(&audio, 0, sizeof(audio));
    audio.output_available = 1;
    snprintf(audio.system_audio_bus, sizeof(audio.system_audio_bus), "%s", fifo);

    /* No upload: the caller is told to use its own built-in sound. */
    call(&audio, "event_sound", "{\"event\":\"timer\"}");
    CHECK(strstr(response, "\"custom\":false") && audio.sample_pid == 0);
    call(&audio, "event_sound", "{\"event\":\"bogus\"}");
    CHECK(strstr(response, "\"ok\":false"));

    /* An uploaded timer sound plays, duplicated to stereo. */
    CHECK(write_raw(path, 3000, 1234) == 0);
    call(&audio, "event_sound", "{\"event\":\"timer\"}");
    CHECK(strstr(response, "\"custom\":true,\"playing\":true") && audio.sample_pid > 0);
    child = audio.sample_pid;
    /* A second ring while it plays is busy, not a fallback to the cue. */
    call(&audio, "event_sound", "{\"event\":\"timer\"}");
    CHECK(strstr(response, "\"custom\":true,\"playing\":false,\"busy\":true"));
    CHECK(drain(fifo_fd, first, 4) == 3000 * 4);
    CHECK(first[0] == 1234 && first[1] == 1234 && first[3] == 1234);
    CHECK(reap(child) == 0);
    child = 0;

    /* Dismissal stops a long ring early, ending on a faded whole frame. */
    CHECK(write_raw(path, LE_EVENT_SOUND_MAX_SAMPLES, 20000) == 0);
    call(&audio, "event_sound", "{\"event\":\"timer\"}");
    CHECK(strstr(response, "\"playing\":true"));
    child = audio.sample_pid;
    call(&audio, "event_sound_stop", "{\"event\":\"wake\"}");
    CHECK(strstr(response, "\"stopped\":false"));        /* only its own event */
    usleep(50000);
    call(&audio, "event_sound_stop", "{\"event\":\"timer\"}");
    CHECK(strstr(response, "\"stopped\":true"));
    {
        long bytes = drain(fifo_fd, NULL, 0);
        CHECK(bytes > 0 && bytes < (long)LE_EVENT_SOUND_MAX_SAMPLES * 4 && bytes % 4 == 0);
    }
    CHECK(reap(child) == 0);
    child = 0;

    /* A file larger than the event allows is never streamed. */
    CHECK(write_raw(path, LE_EVENT_SOUND_MAX_SAMPLES + 1, 1) == 0);
    call(&audio, "event_sound", "{\"event\":\"timer\"}");
    CHECK(strstr(response, "\"custom\":false"));
    unlink(path);

    /* A kept (unselected) sound plays only as a preview... */
    CHECK(write_raw(saved, 480, -42) == 0);
    call(&audio, "event_sound", "{\"event\":\"wake\"}");
    CHECK(strstr(response, "\"custom\":false"));
    call(&audio, "event_sound", "{\"event\":\"wake\",\"preview\":true}");
    CHECK(strstr(response, "\"playing\":true"));
    child = audio.sample_pid;
    CHECK(drain(fifo_fd, first, 4) == 480 * 4 && first[0] == -42);
    CHECK(reap(child) == 0);
    child = 0;
    /* ...and once selected it replaces the wake chirp. */
    snprintf(path, sizeof(path), "%s/sound-wake.raw", directory);
    CHECK(rename(saved, path) == 0);
    call(&audio, "wake_chirp", "{}");
    CHECK(strstr(response, "\"custom\":true") && audio.sample_pid > 0 && audio.cue_pid == 0);
    child = audio.sample_pid;
    CHECK(drain(fifo_fd, first, 4) == 480 * 4 && first[0] == -42);
    CHECK(reap(child) == 0);
    child = 0;
    unlink(path);

    status = 0;
    puts("event sound: owner sound playback, busy ring, stop, size cap, preview and wake chime: ok");
done:
    if (child > 0) {
        kill(child, SIGKILL);
        (void)waitpid(child, NULL, 0);
    }
    if (fifo_fd >= 0)
        close(fifo_fd);
    unlink(fifo);
    unlink(path);
    unlink(saved);
    snprintf(path, sizeof(path), "%s/sound-timer.raw", directory);
    unlink(path);
    rmdir(directory);
    return status;
}
