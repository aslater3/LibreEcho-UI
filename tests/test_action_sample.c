#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

/* Exercise real sample playback through a local FIFO, never ALSA. */
static char sound_directory[128];
#define LE_SOUND_DIR sound_directory
#ifndef LE_AUDIOD_SOURCE
#define LE_AUDIOD_SOURCE "../src/adapter/audiod.c"
#endif
#define main audiod_program_main
#include LE_AUDIOD_SOURCE
#undef main

static int drain_fifo(int fd, unsigned char *prefix, size_t prefix_size)
{
    unsigned char buffer[4096];
    size_t prefix_used = 0;

    for (;;) {
        struct pollfd ready = { fd, POLLIN, 0 };
        ssize_t count;

        if (poll(&ready, 1, 1000) <= 0)
            return -1;
        count = read(fd, buffer, sizeof(buffer));
        if (count == 0)
            return prefix_used == prefix_size ? 0 : -1;
        if (count < 0)
            return -1;
        if (prefix_used < prefix_size) {
            size_t copy = (size_t)count;
            if (copy > prefix_size - prefix_used)
                copy = prefix_size - prefix_used;
            memcpy(prefix + prefix_used, buffer, copy);
            prefix_used += copy;
        }
    }
}

static int write_sample_file(int fd, const int16_t *prefix, size_t prefix_count)
{
    int16_t samples[32768];
    size_t i;

    memset(samples, 0, sizeof(samples));
    for (i = 0; i < prefix_count; ++i)
        samples[i] = prefix[i];
    return write(fd, samples, sizeof(samples)) == (ssize_t)sizeof(samples)
        ? 0 : -1;
}

int main(void)
{
    char directory[] = "/tmp/libreecho-action-sample-XXXXXX";
    char sample_path[256], fifo_path[256], response[1024], request[256];
    const int16_t mono[] = { 0, 1234, -2345, 32767, -32768 };
    int16_t first_stereo[10];
    unsigned char first_output[sizeof(first_stereo)];
    struct audio_hw audio;
    int sample_fd = -1, fifo_fd = -1, status = 1, child_status;
    pid_t child = 0;

    if (!mkdtemp(directory))
        return 1;
    snprintf(sound_directory, sizeof(sound_directory), "%s", directory);
    snprintf(sample_path, sizeof(sample_path), "%s/action-1.raw", directory);
    snprintf(fifo_path, sizeof(fifo_path), "%s/system.fifo", directory);
    sample_fd = open(sample_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (sample_fd < 0 || write_sample_file(sample_fd, mono,
                                            sizeof(mono) / sizeof(mono[0])) < 0)
        goto done;
    close(sample_fd);
    sample_fd = -1;
    if (mkfifo(fifo_path, 0600) < 0)
        goto done;
    fifo_fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
    if (fifo_fd < 0)
        goto done;
    memset(&audio, 0, sizeof(audio));
    audio.output_available = 1;
    snprintf(audio.system_audio_bus, sizeof(audio.system_audio_bus), "%s", fifo_path);

    strcpy(request, "{\"v\":1,\"id\":1,\"cmd\":\"sample\",\"args\":{\"name\":\"action-1\"}}");
    handle_request(&audio, request, response, sizeof(response));
    if (!strstr(response, "\"ok\":true") || audio.sample_pid <= 0)
        goto done;
    child = audio.sample_pid;

    /* A second ordinary sample must not overlap the live writer. */
    strcpy(request, "{\"v\":1,\"id\":2,\"cmd\":\"sample\",\"args\":{\"name\":\"action-1\"}}");
    handle_request(&audio, request, response, sizeof(response));
    if (!strstr(response, "\"ok\":false"))
        goto done;
    if (drain_fifo(fifo_fd, first_output, sizeof(first_output)) < 0)
        goto done;
    memcpy(first_stereo, first_output, sizeof(first_stereo));
    for (size_t i = 0; i < sizeof(mono) / sizeof(mono[0]); ++i)
        if (first_stereo[2 * i] != mono[i] ||
            first_stereo[2 * i + 1] != mono[i])
            goto done;
    if (waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
        goto done;
    child = 0;

    /* A reaped but not-yet-cleared PID is safely reusable. */
    strcpy(request, "{\"v\":1,\"id\":3,\"cmd\":\"sample\",\"args\":{\"name\":\"action-1\"}}");
    handle_request(&audio, request, response, sizeof(response));
    if (!strstr(response, "\"ok\":true") || audio.sample_pid <= 0)
        goto done;
    child = audio.sample_pid;
    if (drain_fifo(fifo_fd, NULL, 0) < 0)
        goto done;
    if (waitpid(child, &child_status, 0) != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
        goto done;
    child = 0;

    /* Keep the full incoming defensive coverage in the normal runner. */
    if (!getenv("LIBREECHO_ACTION_SAMPLE_POSITIVE_ONLY")) {
        strcpy(request, "{\"v\":1,\"id\":2,\"cmd\":\"sample\",\"args\":{\"name\":\"../action-1\"}}");
        handle_request(&audio, request, response, sizeof(response));
        if (!strstr(response, "\"ok\":false")) goto done;
        strcpy(request, "{\"v\":1,\"id\":3,\"cmd\":\"sample\",\"args\":{\"name\":\"missing\"}}");
        handle_request(&audio, request, response, sizeof(response));
        if (!strstr(response, "\"ok\":false")) goto done;
        sample_fd = open(sample_path, O_WRONLY | O_TRUNC);
        if (sample_fd < 0 || write(sample_fd, "x", 1) != 1) goto done;
        close(sample_fd);
        sample_fd = -1;
        strcpy(request, "{\"v\":1,\"id\":4,\"cmd\":\"sample\",\"args\":{\"name\":\"action-1\"}}");
        handle_request(&audio, request, response, sizeof(response));
        if (!strstr(response, "\"ok\":false")) goto done;
    }

    status = 0;
    puts("action sample: configured-bus playback, mono duplication, and child lifecycle: ok");
done:
    if (child > 0) {
        kill(child, SIGKILL);
        (void)waitpid(child, NULL, 0);
    }
    if (sample_fd >= 0)
        close(sample_fd);
    if (fifo_fd >= 0)
        close(fifo_fd);
    unlink(sample_path);
    unlink(fifo_path);
    rmdir(directory);
    return status;
}
