#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

/*
 * Do-not-disturb gate in audiod. While the owner's switch is on, chimes, press
 * tones, wake chirps and action samples are dropped without touching the bus;
 * a priority cue (a ringing timer) still plays; the switch is written to its
 * flag file and read back the way audiod reads it at start. Real request
 * handling runs through the included daemon source, with a local FIFO standing
 * in for the system bus, so nothing reaches ALSA.
 */
static char sound_directory[128];
static char dnd_path[256];
#define LE_SOUND_DIR sound_directory
#define LE_DND_PATH dnd_path
#ifndef LE_AUDIOD_SOURCE
#define LE_AUDIOD_SOURCE "../src/adapter/audiod.c"
#endif
#define main audiod_program_main
#include LE_AUDIOD_SOURCE
#undef main

static int drain_fifo(int fd)
{
    unsigned char buffer[4096];

    for (;;) {
        struct pollfd ready = { fd, POLLIN, 0 };
        ssize_t count;

        if (poll(&ready, 1, 1000) <= 0)
            return -1;
        count = read(fd, buffer, sizeof(buffer));
        if (count == 0)
            return 0;
        if (count < 0)
            return -1;
    }
}

/* Keeps the next cue outside the 200 ms gate so each request is judged alone. */
static void settle(void)
{
    struct timespec pause = { 0, 260 * 1000 * 1000 };

    nanosleep(&pause, NULL);
}

static int dnd_call(struct audio_hw *audio, const char *request, const char *must,
                const char *must_not)
{
    char message[512], response[1024];

    snprintf(message, sizeof(message), "%s", request);
    handle_request(audio, message, response, sizeof(response));
    if (must && !strstr(response, must))
        return -1;
    if (must_not && strstr(response, must_not))
        return -1;
    return 0;
}

int main(void)
{
    char directory[] = "/tmp/libreecho-dnd-audio-XXXXXX";
    char fifo_path[256], sample_path[256];
    const int16_t zero[32768] = { 0 };
    struct audio_hw audio;
    int sample_fd = -1, fifo_fd = -1, status = 1;

    if (!mkdtemp(directory))
        return 1;
    snprintf(sound_directory, sizeof(sound_directory), "%s", directory);
    snprintf(dnd_path, sizeof(dnd_path), "%s/do-not-disturb", directory);
    snprintf(fifo_path, sizeof(fifo_path), "%s/system.fifo", directory);
    snprintf(sample_path, sizeof(sample_path), "%s/action-1.raw", directory);
    sample_fd = open(sample_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (sample_fd < 0 || write(sample_fd, zero, sizeof(zero)) != (ssize_t)sizeof(zero))
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

    /* 1. Switch off: a chime plays as before. */
    if (flag_file_load(dnd_path) != 0 || audio.do_not_disturb != 0)
        goto done;
    if (dnd_call(&audio, "{\"v\":1,\"id\":1,\"cmd\":\"cue\",\"args\":{\"first_hz\":660,\"second_hz\":990,\"ms\":90}}",
             "\"ok\":true", "suppressed") < 0)
        goto done;
    if (drain_fifo(fifo_fd) < 0)
        goto done;
    settle();

    /* 2. Switch on: the owner's choice is persisted and applied. */
    if (dnd_call(&audio, "{\"v\":1,\"id\":2,\"cmd\":\"set_do_not_disturb\",\"args\":{\"enabled\":true}}",
             "\"ok\":true", NULL) < 0)
        goto done;
    if (!audio.do_not_disturb || flag_file_load(dnd_path) != 1)
        goto done;
    if (dnd_call(&audio, "{\"v\":1,\"id\":3,\"cmd\":\"set_do_not_disturb\",\"args\":{\"enabled\":\"maybe\"}}",
             "\"ok\":false", NULL) < 0)
        goto done;

    /* 3. Chimes, press tones, wake chirps and samples are all dropped. */
    if (dnd_call(&audio, "{\"v\":1,\"id\":4,\"cmd\":\"cue\",\"args\":{\"first_hz\":660,\"second_hz\":990,\"ms\":90}}",
             "\"suppressed\":true", NULL) < 0)
        goto done;
    if (dnd_call(&audio, "{\"v\":1,\"id\":5,\"cmd\":\"wake_chirp\"}",
             "\"suppressed\":true", NULL) < 0)
        goto done;
    if (dnd_call(&audio, "{\"v\":1,\"id\":6,\"cmd\":\"sample\",\"args\":{\"name\":\"action-1\"}}",
             "\"suppressed\":true", NULL) < 0)
        goto done;

    /* 4. A priority cue (a ringing alarm) still plays while the switch is on. */
    if (dnd_call(&audio, "{\"v\":1,\"id\":7,\"cmd\":\"cue\",\"args\":{\"first_hz\":660,\"second_hz\":990,\"ms\":90,\"priority\":true}}",
             "\"ok\":true", "suppressed") < 0)
        goto done;
    if (drain_fifo(fifo_fd) < 0)
        goto done;
    settle();

    /* 5. Switch off again: chimes return and the flag file says so. */
    if (dnd_call(&audio, "{\"v\":1,\"id\":8,\"cmd\":\"set_do_not_disturb\",\"args\":{\"enabled\":false}}",
             "\"ok\":true", NULL) < 0)
        goto done;
    if (audio.do_not_disturb || flag_file_load(dnd_path) != 0)
        goto done;
    if (dnd_call(&audio, "{\"v\":1,\"id\":9,\"cmd\":\"cue\",\"args\":{\"first_hz\":660,\"second_hz\":990,\"ms\":90}}",
             "\"ok\":true", "suppressed") < 0)
        goto done;
    if (drain_fifo(fifo_fd) < 0)
        goto done;

    /* The status command reports the switch for the UI and for evidence. */
    if (dnd_call(&audio, "{\"v\":1,\"id\":10,\"cmd\":\"status\"}",
             "\"do_not_disturb\":false", NULL) < 0)
        goto done;

    status = 0;
    puts("do not disturb: chimes, wake chirps and samples dropped, priority cues ring, switch persisted: ok");
done:
    if (fifo_fd >= 0)
        close(fifo_fd);
    if (sample_fd >= 0)
        close(sample_fd);
    unlink(dnd_path);
    unlink(sample_path);
    unlink(fifo_path);
    rmdir(directory);
    return status;
}
