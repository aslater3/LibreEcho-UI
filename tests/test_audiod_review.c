#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#define LE_AIRPLAY_ACTIVE_PATH "/tmp/libreecho-audiod-review-airplay.active"
#define LE_SOUND_DIR "/tmp/libreecho-audiod-review-sounds"
#define main audiod_program_main
#include "../src/adapter/audiod.c"
#undef main

#include <stdio.h>

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "audiod review regression: %s\n", message);
        _exit(1);
    }
}

static void wait_for_exit(pid_t pid)
{
    int i;

    for (i = 0; i < 100; i++) {
        if (kill(pid, 0) < 0 && errno == ESRCH)
            return;
        usleep(1000);
    }
    require_condition(0, "child did not exit");
}

int main(void)
{
    struct audio_hw audio;
    int fd;
    int i;
    pid_t pid;
    int status;
    FILE *sample;

    (void)unlink(LE_AIRPLAY_ACTIVE_PATH);
    (void)unlink(LE_SOUND_DIR "/preview.raw");
    (void)rmdir(LE_SOUND_DIR "/directory.raw");
    (void)rmdir(LE_SOUND_DIR);
    (void)mkdir(LE_SOUND_DIR, 0700);
    sample = fopen(LE_SOUND_DIR "/preview.raw", "wb");
    require_condition(sample != NULL, "could not create sample fixture");
    require_condition(fputc(0, sample) != EOF,
                      "could not write sample fixture");
    require_condition(fclose(sample) == 0, "could not close sample fixture");
    fd = sample_open_fd("preview");
    require_condition(fd >= 0, "readable sample was not opened before playback");
    close(fd);
    require_condition(sample_open_fd("missing") < 0,
                      "missing sample was accepted before playback");
    require_condition(mkdir(LE_SOUND_DIR "/directory.raw", 0700) == 0,
                      "could not create directory sample fixture");
    require_condition(sample_open_fd("directory") < 0,
                      "directory sample was accepted before playback");
    (void)unlink(LE_SOUND_DIR "/preview.raw");
    (void)rmdir(LE_SOUND_DIR "/directory.raw");
    (void)rmdir(LE_SOUND_DIR);

    (void)unlink(LE_AIRPLAY_ACTIVE_PATH);
    require_condition(!airplay_media_active(),
                      "inactive AirPlay marker reported as active");
    fd = open(LE_AIRPLAY_ACTIVE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    require_condition(fd >= 0, "could not create AirPlay marker fixture");
    close(fd);
    require_condition(airplay_media_active(),
                      "regular AirPlay marker was not detected");
    require_condition(unlink(LE_AIRPLAY_ACTIVE_PATH) == 0,
                      "could not remove AirPlay marker fixture");

    memset(&audio, 0, sizeof(audio));
    audio.noise_pid = 0;
    pid = fork();
    require_condition(pid >= 0, "fork for reaper fixture failed");
    if (pid == 0)
        _exit(0);
    audio.noise_pid = pid;
    audio.noise_seconds = 60;
    audio.noise_level = 40;
    for (i = 0; i < 100 && audio.noise_pid != 0; i++) {
        reap_children(&audio);
        usleep(1000);
    }
    require_condition(audio.noise_pid == 0,
                      "generic reaper left tracked noise PID set");
    require_condition(audio.noise_seconds == 0 && audio.noise_level == 0,
                      "generic reaper left stale noise state");

    memset(&audio, 0, sizeof(audio));
    audio.ctl_fd = -1;
    pid = fork();
    require_condition(pid >= 0, "fork for shutdown fixture failed");
    if (pid == 0)
        pause();
    audio.noise_pid = pid;
    audio.noise_seconds = 60;
    audio.noise_level = 40;
    audio.noise_started = 1;
    audio_destroy(&audio);
    require_condition(audio.noise_pid == 0,
                      "audio_destroy left noise PID set");
    require_condition(waitpid(pid, &status, WNOHANG) < 0 && errno == ECHILD,
                      "audio_destroy did not reap noise child");
    wait_for_exit(pid);

    puts("audiod AirPlay ownership and child lifecycle: ok");
    return 0;
}
