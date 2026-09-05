#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

/* Exercise the real sample command through a local FIFO, never ALSA. */
static char sound_directory[128];
#define LE_SOUND_DIR sound_directory
#ifndef LE_AUDIOD_SOURCE
#define LE_AUDIOD_SOURCE "../src/adapter/audiod.c"
#endif
#define main audiod_program_main
#include LE_AUDIOD_SOURCE
#undef main

int main(void)
{
    char directory[] = "/tmp/libreecho-action-sample-XXXXXX";
    char sample_path[256], fifo_path[256], response[1024], request[256];
    const int16_t mono[] = {0, 1234, -2345, 32767, -32768};
    int16_t stereo[10];
    struct audio_hw audio;
    struct pollfd ready;
    int sample_fd = -1, fifo_fd = -1, status = 1, child_status;
    size_t used = 0, i;
    pid_t child;

    if (!mkdtemp(directory)) return 1;
    snprintf(sound_directory, sizeof(sound_directory), "%s", directory);
    snprintf(sample_path, sizeof(sample_path), "%s/action-1.raw", directory);
    snprintf(fifo_path, sizeof(fifo_path), "%s/system.fifo", directory);
    sample_fd = open(sample_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (sample_fd < 0 || write(sample_fd, mono, sizeof(mono)) != sizeof(mono))
        goto done;
    close(sample_fd);
    sample_fd = -1;
    if (mkfifo(fifo_path, 0600) < 0) goto done;
    fifo_fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
    if (fifo_fd < 0) goto done;
    memset(&audio, 0, sizeof(audio));
    audio.output_available = 1;
    snprintf(audio.system_audio_bus, sizeof(audio.system_audio_bus), "%s", fifo_path);
    strcpy(request, "{\"v\":1,\"id\":1,\"cmd\":\"sample\",\"args\":{\"name\":\"action-1\"}}");
    handle_request(&audio, request, response, sizeof(response));
    if (!strstr(response, "\"ok\":true")) {
        fprintf(stderr, "sample command must accept the packaged PCM: %s\n", response);
        goto done;
    }
    ready.fd = fifo_fd;
    ready.events = POLLIN;
    while (used < sizeof(stereo)) {
        ssize_t count;
        if (poll(&ready, 1, 1000) <= 0) goto done;
        count = read(fifo_fd, (unsigned char *)stereo + used, sizeof(stereo) - used);
        if (count <= 0) goto done;
        used += (size_t)count;
    }
    child = waitpid(-1, &child_status, 0);
    if (child < 0 || !WIFEXITED(child_status) || WEXITSTATUS(child_status)) goto done;
    for (i = 0; i < sizeof(mono) / sizeof(mono[0]); ++i)
        if (stereo[2*i] != mono[i] || stereo[2*i+1] != mono[i]) goto done;

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
    /* A live sample writer occupies the only slot; never fork another. */
    sample_fd = open(sample_path, O_WRONLY | O_TRUNC);
    if (sample_fd < 0 || write(sample_fd, mono, sizeof(mono)) != sizeof(mono)) goto done;
    close(sample_fd);
    sample_fd = -1;
    audio.sample_pid = fork();
    if (audio.sample_pid < 0) goto done;
    if (!audio.sample_pid) { for (;;) pause(); }
    if (start_sample(&audio, "action-1") != -1) {
        kill(audio.sample_pid, SIGKILL);
        waitpid(audio.sample_pid, NULL, 0);
        goto done;
    }
    kill(audio.sample_pid, SIGKILL);
    waitpid(audio.sample_pid, NULL, 0);
    audio.sample_pid = 0;
    status = 0;
    puts("action sample: real FIFO duplicated-mono playback and invalid/missing/odd PCM rejection: ok");
done:
    if (sample_fd >= 0) close(sample_fd);
    if (fifo_fd >= 0) close(fifo_fd);
    unlink(sample_path);
    unlink(fifo_path);
    rmdir(directory);
    return status;
}
