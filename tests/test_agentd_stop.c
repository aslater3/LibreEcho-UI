/*
 * The spoken-stop path end to end through the real pieces: agentd's stop
 * handler, its playback worker and play_sentence(), against a mock audio
 * service over a real adapter socket.
 *
 * The regression this pins: a "stop" transcript arriving while the device was
 * speaking used to take the playback lifecycle destructor, which joins the
 * worker before telling the audio service to stop -- so the stop could wait
 * out the worker's 30-second status poll, and every later response failed to
 * begin a turn until agentd restarted. The handler must cancel the turn, issue
 * stop_speech immediately, and leave the worker able to speak again.
 *
 * agentd.c is compiled into this test (its main is renamed) so the static stop
 * handler and play callback are reachable without a second daemon; everything
 * they touch here is real, including the adapter protocol and the worker
 * thread. The audio service is the shared mock in hold-speaking mode, which
 * keeps reporting speaking:true until stop_speech arrives, exactly like a TTS
 * engine with audio still playing.
 */

#define _POSIX_C_SOURCE 200809L

#define main agentd_main
#include "../src/adapter/agentd.c"
#undef main

#include <sys/wait.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        result = 1; \
        goto cleanup; \
    } \
} while (0)

static unsigned long long monotonic_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000ULL +
           (unsigned long long)now.tv_nsec / 1000000ULL;
}

static int adapter_request(const char *socket_path, const char *command,
                           const char *args, char *response, size_t size)
{
    struct le_adapter *adapter =
        le_adapter_connect(socket_path, 3000);
    int result;

    if (!adapter)
        return -1;
    result = le_adapter_call(adapter, command, args, response, size);
    le_adapter_close(adapter);
    return result;
}

static int file_contains(const char *path, const char *needle)
{
    char line[1024];
    FILE *file = fopen(path, "r");
    int found = 0;

    if (!file)
        return 0;
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, needle)) {
            found = 1;
            break;
        }
    }
    fclose(file);
    return found;
}

static int wait_for_contains(const char *path, const char *needle,
                             unsigned int timeout_ms)
{
    struct timespec delay = {0, 10000000L};
    unsigned int i;

    for (i = 0; i < timeout_ms / 10U; ++i) {
        if (file_contains(path, needle))
            return 1;
        nanosleep(&delay, NULL);
    }
    return file_contains(path, needle);
}

static int playback_playing(struct le_voice_playback *playback)
{
    int playing;

    pthread_mutex_lock(&playback->mutex);
    playing = playback->playing;
    pthread_mutex_unlock(&playback->mutex);
    return playing;
}

int main(void)
{
    char directory[] = "/tmp/libreecho-agentd-stop-XXXXXX";
    char audio_socket[256] = "";
    char audio_capture[256] = "";
    char radio_socket[256] = "";
    char first_pcm_path[256] = "";
    char response[LE_ADAPTER_MSG_MAX];
    struct agent_state state;
    struct timespec delay = {0, 10000000L};
    unsigned long long stop_started;
    pid_t audio_child = -1;
    unsigned int i;
    int playback_started = 0;
    int metrics_ready = 0;
    int control_ready = 0;
    int result = 0;

    CHECK(mkdtemp(directory) != NULL);
    snprintf(audio_socket, sizeof(audio_socket),
             "%s/audio.sock", directory);
    snprintf(audio_capture, sizeof(audio_capture),
             "%s/audio.txt", directory);
    snprintf(radio_socket, sizeof(radio_socket),
             "%s/radio-absent.sock", directory);
    snprintf(first_pcm_path, sizeof(first_pcm_path),
             "%s/first-pcm", directory);
    CHECK(setenv("LE_TEST_TTS_HOLD_SPEAKING", "1", 1) == 0);
    audio_child = fork();
    CHECK(audio_child >= 0);
    if (audio_child == 0) {
        execl("./build/mock-audio-adapter",
              "./build/mock-audio-adapter",
              audio_socket, audio_capture, (char *)NULL);
        _exit(127);
    }
    for (i = 0; i < 300 && access(audio_socket, F_OK) != 0; ++i)
        nanosleep(&delay, NULL);
    CHECK(access(audio_socket, F_OK) == 0);

    memset(&state, 0, sizeof(state));
    snprintf(state.audio_socket, sizeof(state.audio_socket),
             "%s", audio_socket);
    snprintf(state.tts_socket, sizeof(state.tts_socket),
             "%s", audio_socket);
    snprintf(state.radio_socket, sizeof(state.radio_socket),
             "%s", radio_socket);
    snprintf(state.tts_first_pcm_file, sizeof(state.tts_first_pcm_file),
             "%s", first_pcm_path);
    snprintf(state.turn_request_id, sizeof(state.turn_request_id),
             "%s", "stop-test");
    CHECK(pthread_mutex_init(&state.metrics_mutex, NULL) == 0);
    metrics_ready = 1;
    CHECK(pthread_mutex_init(&state.control_mutex, NULL) == 0);
    control_ready = 1;
    CHECK(le_voice_playback_start(
              &state.playback, play_sentence, &state) == 0);
    playback_started = 1;

    /* Begin turn -> speaking: the worker is inside play_sentence, held in its
       status poll because the mock keeps reporting speaking:true. */
    CHECK(le_voice_playback_begin_turn(&state.playback) == 0);
    CHECK(le_voice_playback_enqueue(
              &state.playback, "A spoken reply.") == 0);
    CHECK(wait_for_contains(
              audio_capture, "\"text\":\"A spoken reply.\"", 5000));
    CHECK(playback_playing(&state.playback) == 1);

    /*
     * Stop transcript while TTS reports speaking:true. The handler must
     * return without waiting for the sentence in flight, and stop_speech must
     * reach the audio service promptly -- the old path joined the worker
     * first, which here would stall for the full 30-second poll loop.
     */
    stop_started = monotonic_ms();
    CHECK(handle_stop_intent(&state, "stop") == 1);
    CHECK(monotonic_ms() - stop_started < 5000);
    CHECK(wait_for_contains(audio_capture, "command stop_speech", 5000));
    CHECK(monotonic_ms() - stop_started < 5000);

    /* Prompt halt: the cancelled sentence leaves the worker idle. */
    for (i = 0; i < 200 && playback_playing(&state.playback); ++i)
        nanosleep(&delay, NULL);
    CHECK(!playback_playing(&state.playback));
    CHECK(le_voice_playback_wait_idle(&state.playback, 2000) == 0);

    /* The worker survived the stop: a following turn begins and speaks. */
    for (i = 0; i < 100; ++i) {
        if (le_voice_playback_begin_turn(&state.playback) == 0)
            break;
        nanosleep(&delay, NULL);
    }
    CHECK(i < 100);
    CHECK(le_voice_playback_enqueue(
              &state.playback, "A second reply.") == 0);
    CHECK(wait_for_contains(
              audio_capture, "\"text\":\"A second reply.\"", 5000));

    printf("agentd: spoken stop handled in %llums; worker kept speaking: ok\n",
           monotonic_ms() - stop_started);

cleanup:
    if (playback_started) {
        /* Release the mock's speaking hold, then join the worker. */
        (void)adapter_request(audio_socket, "stop_speech", NULL,
                              response, sizeof(response));
        for (i = 0; i < 200 && playback_playing(&state.playback); ++i)
            nanosleep(&delay, NULL);
        le_voice_playback_stop(&state.playback);
    }
    if (metrics_ready)
        pthread_mutex_destroy(&state.metrics_mutex);
    if (control_ready)
        pthread_mutex_destroy(&state.control_mutex);
    if (audio_child > 0) {
        kill(audio_child, SIGTERM);
        waitpid(audio_child, NULL, 0);
    }
    unlink(audio_socket);
    unlink(audio_capture);
    unlink(radio_socket);
    unlink(first_pcm_path);
    rmdir(directory);
    return result;
}
