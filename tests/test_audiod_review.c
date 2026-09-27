#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#define LE_AIRPLAY_ACTIVE_PATH "/tmp/libreecho-audiod-review-airplay.active"
#define LE_AIRPLAY_RESTORE_PATH "/tmp/libreecho-audiod-review-airplay.restore"
#define LE_AIRPLAY_MASTER_ACK_PATH "/tmp/libreecho-audiod-review-airplay.master"
#define LE_MASTER_VOLUME_DIR "/tmp/libreecho-audiod-review-runtime"
#define LE_MASTER_VOLUME_PATH LE_MASTER_VOLUME_DIR "/master.volume"
#define LE_SOUND_DIR "/tmp/libreecho-audiod-review-sounds"
#include <sys/syscall.h>
#include <stdarg.h>
static int review_ioctl(int fd, unsigned long request, ...);
#define ioctl review_ioctl
#define main audiod_program_main
#include "../src/adapter/audiod.c"
#undef main
#undef ioctl

#include <stdio.h>

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "audiod review regression: %s\n", message);
        _exit(1);
    }
}

static int codec_raw, codec_writes, codec_fail;
static int review_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (fd != 100000) return (int)syscall(SYS_ioctl, fd, request, arg);
    if (codec_fail) { errno = EIO; return -1; }
    if (request == SNDRV_CTL_IOCTL_ELEM_READ) {
        ((struct snd_ctl_elem_value *)arg)->value.integer.value[0] = codec_raw;
        return 0;
    }
    if (request == SNDRV_CTL_IOCTL_ELEM_WRITE) {
        codec_raw = (int)((struct snd_ctl_elem_value *)arg)->value.integer.value[0];
        codec_writes++;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

static void fixture_restart_codec(struct audio_hw *audio)
{
    audio->ctl_fd = 100000;
    audio->master.found = audio->master.writable = 1;
    audio->master.info.type = SNDRV_CTL_ELEM_TYPE_INTEGER;
    audio->master.info.value.integer.min = 0;
    audio->master.info.value.integer.max = 151;
    audio->master.info.count = 1;
    snprintf(audio->master.name, sizeof(audio->master.name), "PCM Playback Volume");
}

static void fixture_codec(struct audio_hw *audio)
{
    codec_raw = 0; codec_writes = 0; codec_fail = 0;
    fixture_restart_codec(audio);
}

static void assert_published(int value, const char *message)
{
    char text[8], expected[8];
    int fd = open(LE_MASTER_VOLUME_PATH, O_RDONLY | O_NOFOLLOW);
    ssize_t n;
    require_condition(fd >= 0, message);
    n = read(fd, text, sizeof(text) - 1);
    close(fd);
    require_condition(n > 0, message);
    text[n] = '\0';
    snprintf(expected, sizeof(expected), "%d\n", value);
    require_condition(!strcmp(text, expected), message);
}

static void test_startup_runtime_and_reference(void)
{
    struct audio_hw audio;
    unlink(LE_MASTER_VOLUME_PATH); rmdir(LE_MASTER_VOLUME_DIR);
    memset(&audio, 0, sizeof(audio)); fixture_codec(&audio);
    require_condition(audio_set_volume(&audio, 50) == 0, "cold startup creates runtime directory");
    assert_published(50, "cold startup publishes logical level");
    require_condition(codec_raw == 127 && codec_writes == 1, "one raw unity write");
    require_condition(audio_set_volume(&audio, 25) == 0 && codec_writes == 1,
                      "ordinary callback only publishes software level");
    assert_published(25, "button change published");
    codec_raw = 0;
    refresh_state(&audio);
    require_condition(codec_raw == 127 && codec_writes == 2,
                      "guard restores disturbed physical reference once");
    require_condition(audio_set_volume(&audio, 26) == 0 && codec_writes == 2,
                      "logical change after guard does not rewrite codec");
    assert_published(26, "logical change after guard published");
    unlink(LE_MASTER_VOLUME_PATH); rmdir(LE_MASTER_VOLUME_DIR);
    require_condition(symlink("/tmp", LE_MASTER_VOLUME_DIR) == 0, "symlink runtime fixture");
    require_condition(audio_set_volume(&audio, 31) < 0 && audio.volume == 26,
                      "symlink runtime refuses publication");
    unlink(LE_MASTER_VOLUME_DIR);
    require_condition(audio_set_volume(&audio, 31) == 0,
                      "publication retries when runtime directory returns");
    assert_published(31, "retry publishes requested level");
    codec_fail = 1;
    require_condition(audio_set_volume(&audio, 41) < 0 && audio.volume == 31,
                      "reference becoming unavailable refuses callback before status poll");
    refresh_state(&audio);
    require_condition(audio_set_volume(&audio, 40) < 0 && audio.volume == 31,
                      "unavailable reference refuses logical success");
    assert_published(31, "unavailable reference preserves publication");
    codec_fail = 0;
    audio.reference_ready = 0;
    snprintf(audio.master.name, sizeof(audio.master.name), "Unknown Playback Volume");
    require_condition(audio_set_volume(&audio, 40) < 0 && codec_writes == 2,
                      "unknown control refuses percentage fallback");
    snprintf(audio.master.name, sizeof(audio.master.name), "PCM Playback Volume");
    audio.master.info.value.integer.max = 126;
    require_condition(audio_set_volume(&audio, 40) < 0 && codec_writes == 2,
                      "codec without raw unity refuses unsafe write");
    audio.master.info.value.integer.max = 151;
    require_condition(audio_set_volume(&audio, 40) == 0 && codec_writes == 2,
                      "known already-unity PCM needs no physical write");
    assert_published(40, "already-unity PCM publishes logical gain");
}

static void test_restart_precedence(void)
{
    struct audio_hw audio;
    memset(&audio, 0, sizeof(audio)); fixture_codec(&audio);
    audio.have_card_info = 1;
    require_condition(setenv("LE_AUDIO_VOLUME", "50", 1) == 0, "persisted fixture");
    require_condition(audio_set_volume(&audio, 25) == 0, "runtime 25 fixture");
    audio.volume = 50;
    apply_persisted_levels(&audio);
    require_condition(audio.volume == 25, "runtime 25 outranks persisted 50");
    assert_published(25, "persisted config did not overwrite runtime");
    unlink(LE_MASTER_VOLUME_PATH);
    audio.volume = 25;
    apply_persisted_levels(&audio);
    require_condition(audio.volume == 50, "absent runtime uses persisted config");
    require_condition(access(LE_MASTER_VOLUME_PATH, F_OK) != 0,
                      "persisted fallback selected without premature publication");
    require_condition(audio_set_volume(&audio, audio.volume) == 0,
                      "publish fallback after restoration inspection");
    assert_published(50, "persisted fallback published");
    {
        int fd = open(LE_MASTER_VOLUME_PATH, O_WRONLY | O_TRUNC);
        require_condition(fd >= 0 && write(fd, "bad\n", 4) == 4,
                          "invalid runtime fixture");
        close(fd);
    }
    audio.volume = 42;
    apply_persisted_levels(&audio);
    require_condition(audio.volume == 50, "invalid runtime uses persisted config");
    {
        int fd = open(LE_MASTER_VOLUME_PATH, O_RDONLY);
        char text[8] = {0};
        require_condition(fd >= 0 && read(fd, text, sizeof(text) - 1) == 4 &&
                          !strcmp(text, "bad\n"), "invalid runtime not prematurely overwritten");
        close(fd);
    }
    require_condition(audio_set_volume(&audio, audio.volume) == 0,
                      "publish fallback after invalid runtime inspection");
    assert_published(50, "invalid runtime replaced with persisted fallback");
    require_condition(setenv("LE_AUDIO_VOLUME", "invalid", 1) == 0, "invalid env fixture");
    unlink(LE_MASTER_VOLUME_PATH);
    audio.volume = 42;
    apply_persisted_levels(&audio);
    require_condition(audio.volume == 42 && access(LE_MASTER_VOLUME_PATH, F_OK) != 0,
                      "invalid environment is not published");
    unsetenv("LE_AUDIO_VOLUME");
}

static void test_startup_retry_cadence(void)
{
    struct audio_hw audio;
    memset(&audio, 0, sizeof(audio)); fixture_codec(&audio);
    unlink(LE_MASTER_VOLUME_PATH); rmdir(LE_MASTER_VOLUME_DIR);
    require_condition(symlink("/tmp", LE_MASTER_VOLUME_DIR) == 0, "blocked startup fixture");
    audio.volume = 39; audio.volume_publish_pending = 1;
    retry_startup_volume(&audio, 1000);
    require_condition(audio.volume_publish_pending, "startup failure remains pending");
    require_condition(unlink(LE_MASTER_VOLUME_DIR) == 0, "unblock startup runtime");
    retry_startup_volume(&audio, 1100);
    require_condition(audio.volume_publish_pending && access(LE_MASTER_VOLUME_PATH, F_OK) != 0,
                      "retry does not spin before bounded interval");
    audio.airplay_restore_pending = 1;
    retry_startup_volume(&audio, 1250);
    require_condition(audio.volume_publish_pending, "pending AirPlay restore wins retry order");
    audio.airplay_restore_pending = 0;
    retry_startup_volume(&audio, 1250);
    require_condition(!audio.volume_publish_pending, "retry recovers after runtime returns");
    assert_published(39, "retry publishes startup selection");
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

struct marker_fixture { char label[96], identity[96]; };
static struct marker_fixture markers[24];
static size_t marker_count;

/* Keep the existing ordering cases, but transmit real live marker identities. */
static void activate_marker(const char *label)
{
    struct stat st;
    struct timespec delay = {0, 2000000};
    int fd, n;
    require_condition(marker_count < sizeof(markers) / sizeof(markers[0]), "marker fixture capacity");
    require_condition(unlink(LE_AIRPLAY_ACTIVE_PATH) == 0 || errno == ENOENT,
                      "remove prior marker");
    nanosleep(&delay, NULL);
    fd = open(LE_AIRPLAY_ACTIVE_PATH, O_WRONLY | O_CREAT | O_EXCL, 0600);
    require_condition(fd >= 0 && fstat(fd, &st) == 0, "create regular marker");
    close(fd);
    snprintf(markers[marker_count].label, sizeof(markers[marker_count].label), "%s", label);
    n = snprintf(markers[marker_count].identity, sizeof(markers[marker_count].identity),
                 "%llu:%llu:%lld:%ld", (unsigned long long)st.st_dev,
                 (unsigned long long)st.st_ino, (long long)st.st_ctim.tv_sec,
                 st.st_ctim.tv_nsec);
    require_condition(n > 0 && n < (int)sizeof(markers[marker_count].identity),
                      "canonical marker identity");
    marker_count++;
}

static void mock_audiod_ipc(struct audio_hw *audio, const char *request,
                            char *response, size_t size)
{
    int pair[2];
    char wire[256], translated[384];
    const char *session = strstr(request, "\"session\":\"");
    size_t i;
    ssize_t n;
    /* Replace only the session field; callback IDs remain independent. */
    if (session) {
        const char *value = session + strlen("\"session\":\"");
        const char *end = strchr(value, '"');
        require_condition(end != NULL, "session field terminator");
        for (i = 0; i < marker_count; i++) {
            if (strlen(markers[i].label) == (size_t)(end - value) &&
                !strncmp(value, markers[i].label, (size_t)(end - value))) {
                int written = snprintf(translated, sizeof(translated), "%.*s%s%s",
                                       (int)(value - request), request,
                                       markers[i].identity, end);
                require_condition(written > 0 && written < (int)sizeof(translated),
                                  "translated IPC request fits");
                request = translated;
                break;
            }
        }
    }
    require_condition(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "IPC socketpair");
    require_condition(write(pair[0], request, strlen(request)) == (ssize_t)strlen(request),
                      "IPC request write");
    n = read(pair[1], wire, sizeof(wire) - 1);
    require_condition(n > 0, "IPC request read");
    wire[n] = '\0';
    n = handle_request(audio, wire, response, size);
    require_condition(n > 0 && write(pair[1], response, (size_t)n) == n,
                      "IPC response write");
    n = read(pair[0], response, size - 1);
    require_condition(n > 0, "IPC response read");
    response[n] = '\0';
    close(pair[0]); close(pair[1]);
}

static void test_physical_reference_range(void)
{
    require_condition(speaker_value_for_percent(0, 151, 100) == 127,
                      "codec reference is raw 0 dB, never boosted maximum");
    require_condition(speaker_value_for_percent(0, 126, 100) < 0,
                      "codec lacking unity must not claim an available reference");
}

static void test_logical_restart_input(void)
{
    struct audio_hw audio;
    char victim[] = "/tmp/libreecho-audiod-review-volume-victim";
    int fd;
    unlink(LE_MASTER_VOLUME_PATH); unlink(victim);
    fd = open(LE_MASTER_VOLUME_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    require_condition(fd >= 0 && write(fd, "25\n", 3) == 3, "restart file fixture");
    close(fd);
    memset(&audio, 0, sizeof(audio)); audio.volume = 50;
    require_condition(audio_load_logical_volume(&audio) == 0 && audio.volume == 25,
                      "restart recovers logical volume");
    unlink(LE_MASTER_VOLUME_PATH);
    fd = open(victim, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    require_condition(fd >= 0 && write(fd, "0\n", 2) == 2, "symlink target fixture");
    close(fd);
    require_condition(symlink(victim, LE_MASTER_VOLUME_PATH) == 0, "symlink fixture");
    audio.volume = 50;
    require_condition(audio_load_logical_volume(&audio) < 0 && audio.volume == 50,
                      "restart refuses symlink input");
    unlink(LE_MASTER_VOLUME_PATH); unlink(victim);
    require_condition(mkfifo(LE_MASTER_VOLUME_PATH, 0600) == 0, "FIFO fixture");
    require_condition(audio_load_logical_volume(&audio) < 0 && audio.volume == 50,
                      "restart does not block on FIFO");
    unlink(LE_MASTER_VOLUME_PATH);
}

static void test_logical_master_status(void)
{
    struct audio_hw audio;
    char response[1024], request[] = "{\"v\":1,\"id\":1,\"cmd\":\"status\"}";
    memset(&audio, 0, sizeof(audio)); fixture_codec(&audio);
    require_condition(audio_set_volume(&audio, 25) == 0, "set logical level");
    assert_published(25, "published pre-DSP logical level");
    require_condition(codec_raw == 127 && codec_writes == 1,
                      "codec stays at raw physical reference");
    mock_audiod_ipc(&audio, request, response, sizeof(response));
    require_condition(strstr(response, "\"volume\":25") != NULL,
                      "status uses logical master without codec readback");
    audio.have_card_info = 1;
    snprintf(audio.pcm_path, sizeof(audio.pcm_path), "%s", LE_MASTER_VOLUME_PATH);
    mock_audiod_ipc(&audio, request, response, sizeof(response));
    require_condition(strstr(response, "\"output_available\":true") != NULL,
                      "known PCM reference and present output report available");
    codec_fail = 1;
    mock_audiod_ipc(&audio, request, response, sizeof(response));
    require_condition(strstr(response, "\"volume\":25") &&
                      strstr(response, "\"output_available\":false"),
                      "missing physical reference reports unavailable, not fabricated level");
    codec_fail = 0;
    unlink(LE_MASTER_VOLUME_PATH);
}

/* Drive real audiod request parsing with a host-only amixer recorder. */
static void test_airplay_restore(void)
{
    struct audio_hw audio;
    char request[256], response[1024];
    FILE *file;
    memset(&audio, 0, sizeof(audio)); fixture_codec(&audio);
    audio.volume = 42;
    audio.requested_volume = 42;
#define REQUEST(text) do { \
    snprintf(request, sizeof(request), "{\"v\":1,\"id\":1,\"cmd\":%s}", text); \
    mock_audiod_ipc(&audio, request, response, sizeof(response)); \
    require_condition(strstr(response, "\"ok\":true") != NULL, text); \
} while (0)
#define REJECT(text) do { \
    snprintf(request, sizeof(request), "{\"v\":1,\"id\":1,\"cmd\":%s}", text); \
    mock_audiod_ipc(&audio, request, response, sizeof(response)); \
    require_condition(strstr(response, "\"ok\":false") != NULL, text); \
} while (0)
    activate_marker("1:1:1:1");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:1:1:1\",\"callback\":\"1:1:1:1\",\"volume\":0}");
    require_condition(audio.volume == 0, "sender mute");
    assert_published(0, "sender mute published zero software gain");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:1:1:1\"}");
    require_condition(audio.volume == 42, "stop restores pre-playback master");
    assert_published(42, "stop publishes pre-playback master");
    REJECT("\"airplay_volume\",\"args\":{\"session\":\"1:1:1:1\",\"callback\":\"1:2:1:2\",\"volume\":0}");
    require_condition(audio.volume == 42, "ended marker cannot be reacknowledged");
    activate_marker("1:2:2:1");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:2:2:1\",\"callback\":\"1:2:2:1\",\"volume\":0}");
    REQUEST("\"set_volume\",\"args\":{\"volume\":25}");
    assert_published(25, "button override publishes 25");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:2:2:1\",\"callback\":\"1:3:2:2\",\"volume\":70}");
    assert_published(70, "new sender callback changes playback without rebasing button baseline");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:2:2:1\"}");
    require_condition(audio.volume == 25, "latest local choice survives later sender callback");
    assert_published(25, "stop retains button override in software gain");
    activate_marker("1:4:3:1");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:4:3:1\",\"callback\":\"1:4:3:1\",\"volume\":10}");
    activate_marker("1:5:4:1");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:5:4:1\",\"callback\":\"1:5:4:1\",\"volume\":60}");
    assert_published(60, "replacement sender publishes 60");
    file = fopen(LE_AIRPLAY_MASTER_ACK_PATH, "w");
    require_condition(file != NULL && fclose(file) == 0, "replacement ack fixture");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:4:3:1\"}");
    require_condition(audio.volume == 60 && access(LE_AIRPLAY_MASTER_ACK_PATH, F_OK) == 0,
                      "old session cannot end or revoke replacement");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:5:4:1\"}");
    require_condition(access(LE_AIRPLAY_MASTER_ACK_PATH, F_OK) != 0,
                      "replacement end revokes its acknowledgment");
    require_condition(audio.volume == 25, "replacement restores baseline");
    assert_published(25, "replacement stop publishes baseline");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:5:4:1\"}");
    require_condition(audio.volume == 25, "duplicate end is harmless");
    snprintf(request, sizeof(request),
             "{\"v\":1,\"id\":1,\"cmd\":\"airplay_volume\",\"args\":{\"session\":\"bad/one\",\"volume\":80}}");
    mock_audiod_ipc(&audio, request, response, sizeof(response));
    require_condition(strstr(response, "\"ok\":false") && audio.volume == 25,
                      "invalid session rejected without mixer write");
    /* The first response is deliberately discarded by the caller: audiod
     * already applied it, so a retry cannot undo a later local choice. */
    activate_marker("1:6:5:1");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:6:5:1\",\"callback\":\"1:6:5:1\",\"volume\":0}");
    REQUEST("\"set_volume\",\"args\":{\"volume\":31}");
    assert_published(31, "mute followed by local button publishes 31");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:6:5:1\",\"callback\":\"1:6:5:1\",\"volume\":0}");
    require_condition(audio.volume == 31, "lost response retry preserves button");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:6:5:1\",\"callback\":\"1:7:5:3\",\"volume\":65}");
    REJECT("\"airplay_volume\",\"args\":{\"session\":\"1:6:5:1\",\"callback\":\"1:6:5:1\",\"volume\":0}");
    require_condition(audio.volume == 65, "old retry cannot supersede new callback");
    assert_published(65, "new sender callback publishes 65");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:6:5:1\"}");
    require_condition(audio.volume == 31, "stop after unknown response restores local choice");
    assert_published(31, "stop after lost reply publishes 31");
    REJECT("\"airplay_volume\",\"args\":{\"session\":\"1:6:5:1\",\"callback\":\"1:8:5:4\",\"volume\":0}");
    require_condition(audio.volume == 31, "late delivery after stop cannot remute");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:9:6:1\"}");
    REJECT("\"airplay_volume\",\"args\":{\"session\":\"1:9:6:1\",\"callback\":\"1:9:6:1\",\"volume\":0}");
    require_condition(audio.volume == 31, "end before first delivery tombstones session");
    snprintf(request, sizeof(request),
             "{\"v\":1,\"id\":1,\"cmd\":\"airplay_volume\",\"args\":{\"session\":\"badcallback\",\"callback\":\"1:2:3:1000000000\",\"volume\":0}}");
    mock_audiod_ipc(&audio, request, response, sizeof(response));
    require_condition(strstr(response, "\"ok\":false") && audio.volume == 31,
                      "out-of-range callback timestamp rejected");
    /* No request was delivered for this callback before this first retry. */
    activate_marker("1:10:7:1");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:10:7:1\",\"callback\":\"1:10:7:1\",\"volume\":18}");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:10:7:1\",\"callback\":\"1:10:7:1\",\"volume\":18}");
    require_condition(audio.volume == 18, "undelivered first write applies exactly once");
    assert_published(18, "sender retry leaves exact 18 published");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:10:7:1\"}");
    /* A ends, B starts and ends: neither a late A write nor an A end may
     * resurrect the old session or invalidate B's tombstone. */
    activate_marker("1:11:8:1");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:11:8:1\",\"callback\":\"1:11:8:2\",\"volume\":0}");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:11:8:1\"}");
    activate_marker("1:12:9:1");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:12:9:1\",\"callback\":\"1:12:9:2\",\"volume\":60}");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:12:9:1\"}");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:11:8:1\"}");
    REJECT("\"airplay_volume\",\"args\":{\"session\":\"1:11:8:1\",\"callback\":\"1:11:8:3\",\"volume\":0}");
    require_condition(audio.volume == 31, "A cannot remute after B ends");
    REJECT("\"airplay_volume\",\"args\":{\"session\":\"1:12:9:1\",\"callback\":\"1:12:9:3\",\"volume\":0}");
    require_condition(audio.volume == 31, "B remains ended after late A end");
    /* A delayed callback must not displace an active newer session. */
    activate_marker("1:13:10:1");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:13:10:1\",\"callback\":\"1:13:10:2\",\"volume\":64}");
    REJECT("\"airplay_volume\",\"args\":{\"session\":\"1:11:8:1\",\"callback\":\"1:11:8:4\",\"volume\":0}");
    require_condition(audio.volume == 64, "delayed A cannot replace active C");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:13:10:1\"}");
    require_condition(audio.volume == 31, "newer C restores local baseline");
    /* Unknown write result: end arrives before the first volume is delivered.
     * Even after another end advances the watermark, the late write is stale. */
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:14:11:1\"}");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:15:12:1\"}");
    REJECT("\"airplay_volume\",\"args\":{\"session\":\"1:14:11:1\",\"callback\":\"1:14:11:2\",\"volume\":0}");
    require_condition(audio.volume == 31, "unknown end then late volume stays stale");
    activate_marker("1:16:13:1");
    REQUEST("\"airplay_volume\",\"args\":{\"session\":\"1:16:13:1\",\"callback\":\"1:16:13:2\",\"volume\":76}");
    require_condition(audio.volume == 76, "newer session after unknown end still works");
    REJECT("\"airplay_volume\",\"args\":{\"session\":\"1:17:12:1\",\"callback\":\"1:17:12:2\",\"volume\":0}");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:17:13:1\"}");
    require_condition(audio.volume == 76, "clock rollback and equal-time collision fail closed");
    REQUEST("\"airplay_end\",\"args\":{\"session\":\"1:16:13:1\"}");
    require_condition(audio.volume == 31, "newer session restores local choice");
    snprintf(request, sizeof(request),
             "{\"v\":1,\"id\":1,\"cmd\":\"airplay_end\",\"args\":{\"session\":\"1:99:014:1\"}}");
    mock_audiod_ipc(&audio, request, response, sizeof(response));
    require_condition(strstr(response, "\"ok\":false") && audio.volume == 31,
                      "noncanonical session rejected without state change");
    snprintf(request, sizeof(request),
             "{\"v\":1,\"id\":1,\"cmd\":\"airplay_end\",\"args\":{\"session\":\"1:99:9223372036854775808:1\"}}");
    mock_audiod_ipc(&audio, request, response, sizeof(response));
    require_condition(strstr(response, "\"ok\":false") && audio.volume == 31,
                      "overflowed session timestamp rejected");
#undef REQUEST
    require_condition(codec_writes == 1 && codec_raw == 127,
                      "AirPlay callbacks never rewrite physical reference");
}

static void test_airplay_restart_recovery(void)
{
    struct audio_hw audio, restarted, fresh;
    char request[256], response[1024];
    FILE *file;
    int controller;
    char controller_path[128];
    const char *sender = "\"airplay_volume\",\"args\":{\"session\":\"1:22:100:1\",\"callback\":\"1:23:100:2\",\"volume\":0}";
    char dir[] = "/tmp/libreecho-restart-XXXXXX";
    require_condition(mkdtemp(dir) != NULL, "restart fixture");
    unlink(LE_AIRPLAY_RESTORE_PATH);
    activate_marker("1:22:100:1");
    memset(&audio, 0, sizeof(audio)); fixture_codec(&audio);
    audio.volume = audio.requested_volume = 42;
#define RESTART_REQUEST(a, text) do { \
    snprintf(request, sizeof(request), "{\"v\":1,\"id\":1,\"cmd\":%s}", text); \
    mock_audiod_ipc(a, request, response, sizeof(response)); \
    require_condition(strstr(response, "\"ok\":true") != NULL, text); \
} while (0)
#define RESTART_REJECT(a, text) do { \
    snprintf(request, sizeof(request), "{\"v\":1,\"id\":1,\"cmd\":%s}", text); \
    mock_audiod_ipc(a, request, response, sizeof(response)); \
    require_condition(strstr(response, "\"ok\":false") != NULL, text); \
} while (0)
    RESTART_REQUEST(&audio, sender);
    require_condition(audio.volume == 0, "sender muted mixer");
    memset(&restarted, 0, sizeof(restarted)); fixture_restart_codec(&restarted);
    restarted.volume = restarted.requested_volume = 42;
    restarted.have_card_info = 1;
    require_condition(setenv("LE_AUDIO_VOLUME", "50", 1) == 0, "stale restart config");
    apply_persisted_levels(&restarted);
    assert_published(0, "active sender runtime outranks stale config");
    airplay_restore_load(&restarted);
    require_condition(restarted.volume == 0 && restarted.airplay_baseline == 42,
                      "active restart recovers sender and original local baseline");
    assert_published(0, "active restart retains sender mute publication");
    /* SIGKILL leaves the bridge and its marker alive, but kills the controller socket. */
    snprintf(restarted.airplay_controller_socket, sizeof(restarted.airplay_controller_socket),
             "%s/controller.sock", dir);
    file = fopen(LE_AIRPLAY_MASTER_ACK_PATH, "w");
    require_condition(file != NULL && fclose(file) == 0, "orphan ack fixture");
    airplay_restore_poll(&restarted);
    require_condition(access(LE_AIRPLAY_MASTER_ACK_PATH, F_OK) != 0,
                      "orphan sender media gate revoked");
    require_condition(restarted.volume == 42 && !restarted.airplay_session[0],
                      "controller death with orphan marker restores alarms");
    assert_published(42, "controller death publishes baseline");
    /* A controller restarted against the orphan marker must not get a success
     * response or publish a fresh master acknowledgment after restoration. */
    RESTART_REJECT(&restarted, sender);
    require_condition(restarted.volume == 42 && access(LE_AIRPLAY_MASTER_ACK_PATH, F_OK) != 0,
                      "orphan callback refused without republishing acknowledgment");
    memset(&fresh, 0, sizeof(fresh)); fixture_restart_codec(&fresh);
    fresh.volume = fresh.requested_volume = 42;
    airplay_restore_load(&fresh);
    RESTART_REJECT(&fresh, sender);
    require_condition(fresh.volume == 42 && access(LE_AIRPLAY_MASTER_ACK_PATH, F_OK) != 0,
                      "orphan callback after audiod restart refused with marker present");
    /* Controller dies without an end; audiod's own clock must restore. */
    unlink(LE_AIRPLAY_ACTIVE_PATH);
    airplay_restore_poll(&restarted);
    require_condition(restarted.volume == 42 && !restarted.airplay_session[0],
                      "restart plus controller death restores baseline");
    airplay_restore_poll(&audio);
    require_condition(audio.volume == 42 && !audio.airplay_session[0], "controller death restores baseline");
    /* The ended watermark survives restart even without a live marker. */
    memset(&restarted, 0, sizeof(restarted)); fixture_restart_codec(&restarted);
    restarted.volume = restarted.requested_volume = 42;
    airplay_restore_load(&restarted);
    RESTART_REJECT(&restarted, sender);
    require_condition(restarted.volume == 42 && restarted.airplay_newest_ended &&
                      !restarted.airplay_session[0],
                      "late write after audiod restart cannot recreate active snapshot");
    /* Restart while the original callback is acknowledged; local choice wins. */
    snprintf(controller_path, sizeof(controller_path), "%s/controller.sock", dir);
    controller = le_adapter_listen(controller_path);
    require_condition(controller >= 0, "live controller fixture");
    activate_marker("1:24:101:1");
    memset(&fresh, 0, sizeof(fresh)); fixture_restart_codec(&fresh);
    fresh.volume = fresh.requested_volume = 42;
    RESTART_REJECT(&fresh, sender);
    require_condition(fresh.volume == 42, "old callback refused with different live marker");
    RESTART_REQUEST(&audio, "\"airplay_volume\",\"args\":{\"session\":\"1:24:101:1\",\"callback\":\"1:25:101:2\",\"volume\":0}");
    RESTART_REQUEST(&audio, "\"set_volume\",\"args\":{\"volume\":27}");
    memset(&restarted, 0, sizeof(restarted)); fixture_restart_codec(&restarted);
    restarted.volume = restarted.requested_volume = 0;
    snprintf(restarted.airplay_controller_socket, sizeof(restarted.airplay_controller_socket),
             "%s", controller_path);
    airplay_restore_load(&restarted);
    require_condition(restarted.airplay_baseline == 27 && restarted.airplay_session[0] &&
                      restarted.volume == 27, "restart retains local override");
    assert_published(27, "restart retains exact local override publication");
    RESTART_REQUEST(&restarted, "\"airplay_volume\",\"args\":{\"session\":\"1:24:101:1\",\"callback\":\"1:25:101:2\",\"volume\":0}");
    require_condition(restarted.volume == 27, "acknowledged callback not replayed");
    airplay_restore_poll(&restarted);
    require_condition(restarted.airplay_session[0], "live controller does not end session");
    close(controller); unlink(controller_path);
    airplay_restore_poll(&restarted);
    require_condition(restarted.volume == 27 && !restarted.airplay_session[0],
                      "controller death preserves latest button with orphan marker");
    memset(&fresh, 0, sizeof(fresh)); fixture_restart_codec(&fresh);
    fresh.volume = fresh.requested_volume = 27;
    airplay_restore_load(&fresh);
    RESTART_REJECT(&fresh, "\"airplay_volume\",\"args\":{\"session\":\"1:24:101:1\",\"callback\":\"1:26:101:3\",\"volume\":0}");
    require_condition(fresh.volume == 27, "ended restart rejects late callback with marker present");
    activate_marker("1:27:102:1");
    RESTART_REQUEST(&fresh, "\"airplay_volume\",\"args\":{\"session\":\"1:27:102:1\",\"callback\":\"1:27:102:2\",\"volume\":60}");
    require_condition(fresh.volume == 60, "newer marker works after persisted end");
    file = fopen(LE_AIRPLAY_MASTER_ACK_PATH, "w");
    require_condition(file != NULL && fclose(file) == 0, "live acknowledgment fixture");
    RESTART_REQUEST(&fresh, "\"airplay_end\",\"args\":{\"session\":\"1:27:102:1\"}");
    require_condition(access(LE_AIRPLAY_MASTER_ACK_PATH, F_OK) != 0,
                      "explicit end revokes acknowledgment while marker remains live");
    memset(&fresh, 0, sizeof(fresh)); fixture_restart_codec(&fresh);
    fresh.volume = fresh.requested_volume = 27;
    airplay_restore_load(&fresh);
    RESTART_REJECT(&fresh, "\"airplay_volume\",\"args\":{\"session\":\"1:27:102:1\",\"callback\":\"1:28:102:3\",\"volume\":0}");
    require_condition(fresh.volume == 27 && fresh.airplay_newest_ended,
                      "explicit end survives restart with marker present");
    activate_marker("1:29:103:1");
    RESTART_REQUEST(&fresh, "\"airplay_volume\",\"args\":{\"session\":\"1:29:103:1\",\"callback\":\"1:29:103:2\",\"volume\":55}");
    require_condition(fresh.volume == 55, "newer session works after explicit end restart");
    RESTART_REQUEST(&fresh, "\"airplay_end\",\"args\":{\"session\":\"1:29:103:1\"}");
    unlink(LE_AIRPLAY_ACTIVE_PATH);
    airplay_restore_poll(&restarted);
    require_condition(restarted.volume == 27, "restart stop restores local override");
    RESTART_REQUEST(&restarted, "\"airplay_end\",\"args\":{\"session\":\"1:24:101:1\"}");
    /* A failed mixer restore must retain both the end and its baseline
     * through another restart; no callback can replace pending restoration. */
    activate_marker("1:30:104:1");
    RESTART_REQUEST(&fresh, "\"airplay_volume\",\"args\":{\"session\":\"1:30:104:1\",\"callback\":\"1:30:104:2\",\"volume\":0}");
    file = fopen(LE_AIRPLAY_MASTER_ACK_PATH, "w");
    require_condition(file != NULL && fclose(file) == 0, "pending acknowledgment fixture");
    codec_fail = 1; fresh.reference_ready = 0;
    RESTART_REJECT(&fresh, "\"airplay_end\",\"args\":{\"session\":\"1:30:104:1\"}");
    require_condition(fresh.airplay_restore_pending &&
                      access(LE_AIRPLAY_MASTER_ACK_PATH, F_OK) != 0,
                      "pending restoration revokes media acknowledgment");
    memset(&fresh, 0, sizeof(fresh)); fixture_restart_codec(&fresh);
    fresh.volume = fresh.requested_volume = 0;
    fresh.have_card_info = 1;
    apply_persisted_levels(&fresh);
    assert_published(0, "pending sender runtime outranks stale config");
    airplay_restore_load(&fresh);
    require_condition(fresh.airplay_restore_pending && fresh.airplay_baseline == 27,
                      "pending baseline survives restart during mixer outage");
    assert_published(0, "pending restore has not fabricated 27 publication");
    RESTART_REJECT(&fresh, "\"airplay_volume\",\"args\":{\"session\":\"1:30:104:1\",\"callback\":\"1:31:104:3\",\"volume\":0}");
    activate_marker("1:33:105:1");
    RESTART_REJECT(&fresh, "\"airplay_volume\",\"args\":{\"session\":\"1:33:105:1\",\"callback\":\"1:33:105:2\",\"volume\":50}");
    RESTART_REJECT(&fresh, "\"set_volume\",\"args\":{\"volume\":12}");
    require_condition(fresh.airplay_restore_pending && fresh.airplay_baseline == 27,
                      "pending restoration cannot lose its baseline");
    codec_fail = 0;
    airplay_restore_poll(&fresh);
    require_condition(fresh.volume == 27 && !fresh.airplay_restore_pending,
                      "pending restoration completes when mixer returns");
    assert_published(27, "pending restore publishes exact baseline");
    RESTART_REJECT(&fresh, "\"airplay_volume\",\"args\":{\"session\":\"1:30:104:1\",\"callback\":\"1:32:104:4\",\"volume\":0}");
    RESTART_REQUEST(&fresh, "\"airplay_volume\",\"args\":{\"session\":\"1:33:105:1\",\"callback\":\"1:33:105:2\",\"volume\":50}");
    RESTART_REQUEST(&fresh, "\"airplay_end\",\"args\":{\"session\":\"1:33:105:1\"}");
    unlink(LE_AIRPLAY_ACTIVE_PATH);
    /* Even a matching stat identity of a directory is not a live marker. */
    require_condition(mkdir(LE_AIRPLAY_ACTIVE_PATH, 0700) == 0,
                      "nonregular marker fixture");
    {
        struct stat directory;
        require_condition(stat(LE_AIRPLAY_ACTIVE_PATH, &directory) == 0,
                          "nonregular marker identity");
        snprintf(request, sizeof(request),
                 "{\"v\":1,\"id\":1,\"cmd\":\"airplay_volume\",\"args\":{\"session\":\"%llu:%llu:%lld:%ld\",\"callback\":\"1:30:200:1\",\"volume\":0}}",
                 (unsigned long long)directory.st_dev, (unsigned long long)directory.st_ino,
                 (long long)directory.st_ctim.tv_sec, directory.st_ctim.tv_nsec);
        memset(&fresh, 0, sizeof(fresh)); fixture_restart_codec(&fresh);
        fresh.volume = fresh.requested_volume = 27;
        mock_audiod_ipc(&fresh, request, response, sizeof(response));
        require_condition(strstr(response, "\"ok\":false") && fresh.volume == 27,
                          "directory cannot authorize callback");
    }
    require_condition(rmdir(LE_AIRPLAY_ACTIVE_PATH) == 0, "remove nonregular marker");
    /* Corrupt state must never become a guessed baseline or arbitrary write. */
    file = fopen(LE_AIRPLAY_RESTORE_PATH, "w");
    require_condition(file != NULL, "bad snapshot fixture");
    fputs("1 999:999:999:999 999:999:999:999 200 0\n", file);
    require_condition(fclose(file) == 0, "bad snapshot written");
    memset(&restarted, 0, sizeof(restarted)); fixture_restart_codec(&restarted);
    restarted.volume = restarted.requested_volume = 42;
    airplay_restore_load(&restarted);
    require_condition(!restarted.airplay_session[0] && restarted.volume == 42,
                      "malformed snapshot cannot change mixer");
    unlink(LE_AIRPLAY_ACTIVE_PATH); unlink(LE_AIRPLAY_RESTORE_PATH);
    rmdir(dir);
    unsetenv("LE_AUDIO_VOLUME");
#undef RESTART_REQUEST
}

static void test_pending_restore_beats_stale_config(void)
{
    struct audio_hw audio;
    memset(&audio, 0, sizeof(audio)); fixture_codec(&audio);
    audio.have_card_info = 1;
    audio.volume = 42;
    unlink(LE_MASTER_VOLUME_PATH);
    require_condition(setenv("LE_AUDIO_VOLUME", "50", 1) == 0, "pending stale config");
    require_condition(airplay_ended_save("1:40:200:1", 27, 1) == 0,
                      "pending snapshot fixture");
    apply_persisted_levels(&audio);
    require_condition(audio.volume == 50 && access(LE_MASTER_VOLUME_PATH, F_OK) != 0,
                      "stale env selected but not published before pending snapshot");
    audio.volume_publish_pending = 1;
    airplay_restore_load(&audio);
    require_condition(!audio.airplay_restore_pending && audio.volume == 27,
                      "pending restoration outranks absent runtime and stale env");
    assert_published(27, "pending restoration publishes baseline first");
    unsetenv("LE_AUDIO_VOLUME");
    unlink(LE_AIRPLAY_RESTORE_PATH);
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
    require_condition(fputc(0, sample) != EOF,
                      "could not complete sample fixture");
    require_condition(fclose(sample) == 0, "could not close sample fixture");
    fd = sample_open_fd("preview");
    require_condition(fd >= 0, "readable sample was not opened before playback");
    close(fd);
    sample = fopen(LE_SOUND_DIR "/empty.raw", "wb");
    require_condition(sample != NULL, "could not create empty sample fixture");
    require_condition(fclose(sample) == 0, "could not close empty sample fixture");
    require_condition(sample_open_fd("empty") < 0,
                      "empty sample was accepted before playback");
    sample = fopen(LE_SOUND_DIR "/odd.raw", "wb");
    require_condition(sample != NULL, "could not create odd sample fixture");
    require_condition(fputc(0, sample) != EOF,
                      "could not write odd sample fixture");
    require_condition(fclose(sample) == 0, "could not close odd sample fixture");
    require_condition(sample_open_fd("odd") < 0,
                      "odd-byte sample was accepted before playback");
    (void)unlink(LE_SOUND_DIR "/empty.raw");
    (void)unlink(LE_SOUND_DIR "/odd.raw");
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

    test_physical_reference_range();
    test_startup_runtime_and_reference();
    test_startup_retry_cadence();
    test_restart_precedence();
    test_logical_restart_input();
    test_logical_master_status();
    test_airplay_restore();
    test_airplay_restart_recovery();
    test_pending_restore_beats_stale_config();
    require_condition(unlink(LE_MASTER_VOLUME_PATH) == 0, "remove published fixture");
    require_condition(rmdir(LE_MASTER_VOLUME_DIR) == 0, "remove runtime fixture");
    puts("audiod AirPlay ownership and child lifecycle: ok");
    return 0;
}
