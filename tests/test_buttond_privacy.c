#define _POSIX_C_SOURCE 200809L

/* Host regression for the kernel-owned privacy latch synchronizer. */
#include "buttond_fixture.h"
#define open buttond_fixture_open
#define write(fd, buffer, count) buttond_fixture_write(fd, buffer, count)
/* No retry window: a probe deferred by a missing attribute must be retried on
   the next call, not after a real 30-second wait. */
#define LAMP_PROBE_RETRY_MS 0
static char test_privacy_path[256], test_privacy_fallback_path[256];
static char test_lamp_path[256];
static char test_config_path[256];
static char test_status_path[256], test_status_tmp_path[256];
#define BUTTOND_PRIVACY_STATE_PATH test_privacy_path
#define BUTTOND_MUTE_LAMP_PATH test_lamp_path
#define BUTTOND_PRIVACY_STATE_FALLBACK_PATH test_privacy_fallback_path
#define LE_BUTTOND_CONFIG test_config_path
#define STATUS_PATH test_status_path
#define STATUS_TMP_PATH test_status_tmp_path
#define main buttond_program_main
#include "../src/adapter/buttond.c"
#undef main
#undef open
#undef write

#include <assert.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static int make_listener(const char *path)
{
    struct sockaddr_un address;
    int fd;
    size_t n = strlen(path);

    assert(n < sizeof(address.sun_path));
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, n + 1);
    unlink(path);
    assert(bind(fd, (struct sockaddr *)&address,
                (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1)) == 0);
    assert(listen(fd, 8) == 0);
    return fd;
}

static void mock_audio(int listen_fd, const char *log_path)
{
    FILE *log = fopen(log_path, "w");
    int muted = 0;

    assert(log != NULL);
    for (;;) {
        char request[4096];
        char response[512];
        ssize_t used = 0;
        int fd = accept(listen_fd, NULL, NULL);

        if (fd < 0)
            _exit(2);
        while (used < (ssize_t)sizeof(request) - 1) {
            ssize_t n = read(fd, request + used,
                             sizeof(request) - 1 - (size_t)used);
            if (n <= 0)
                break;
            used += n;
            if (memchr(request, '\n', (size_t)used))
                break;
        }
        request[used > 0 ? used : 0] = '\0';
        fputs(request, log);
        fflush(log);
        if (strstr(request, "\"cmd\":\"set_mute\""))
            muted = strstr(request, "\"muted\":true") != NULL;
        if (strstr(request, "\"cmd\":\"status\""))
            snprintf(response, sizeof(response),
                     "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{\"volume\":50,\"muted\":%s}}\n",
                     muted ? "true" : "false");
        else
            snprintf(response, sizeof(response),
                     "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{}}\n");
        assert(write(fd, response, strlen(response)) == (ssize_t)strlen(response));
        close(fd);
    }
}

static void mock_led(int listen_fd)
{
    for (;;) {
        char request[4096];
        const char response[] =
            "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{}}\n";
        ssize_t used = 0;
        int fd = accept(listen_fd, NULL, NULL);

        if (fd < 0)
            _exit(2);
        while (used < (ssize_t)sizeof(request) - 1) {
            ssize_t n = read(fd, request + used,
                             sizeof(request) - 1 - (size_t)used);
            if (n <= 0)
                break;
            used += n;
            if (memchr(request, '\n', (size_t)used))
                break;
        }
        assert(write(fd, response, strlen(response)) == (ssize_t)strlen(response));
        close(fd);
    }
}

static void write_state(const char *path, int state)
{
    FILE *file = fopen(path, "w");

    assert(file != NULL);
    assert(fprintf(file, "%d\n", state) > 0);
    assert(fclose(file) == 0);
}

/* Read one integer field out of the published capability status record. */
static int status_field(const char *name)
{
    char data[512], needle[32], *found;
    FILE *file = fopen(STATUS_PATH, "r");
    size_t n;

    assert(file != NULL);
    n = fread(data, 1, sizeof(data) - 1, file);
    data[n] = '\0';
    assert(fclose(file) == 0);
    snprintf(needle, sizeof(needle), "%s=", name);
    found = strstr(data, needle);
    assert(found != NULL);
    return atoi(found + strlen(needle));
}

static int log_count(const char *path, const char *needle)
{
    char data[32768];
    FILE *file = fopen(path, "r");
    size_t n;
    int count = 0;
    char *p;

    if (!file)
        return 0;
    n = fread(data, 1, sizeof(data) - 1, file);
    fclose(file);
    data[n] = '\0';
    for (p = data; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        ++count;
    return count;
}

static void init_context(struct context *ctx, const char *audio_socket)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->audio_sock = audio_socket;
    ctx->led_sock = "/tmp/libreecho-buttond-privacy-no-led.sock";
    ctx->volume = 50;
    ctx->muted = 0;
    ctx->step = 5;
    ctx->hold_ms = 1000;
    ctx->brightness = 70;
    ctx->indicated_mute = -1;
    ctx->tones = 0;
    ctx->mute_brightness = 60;
    ctx->privacy_state = -1;
    ctx->privacy_observed = -1;
    ctx->lamp_supported = -1;
    ctx->lamp_retry_at_ms = 0;
}

int main(void)
{
    char directory[] = "/tmp/libreecho-buttond-privacy-XXXXXX";
    char audio_socket[256], led_socket[256];
    char audio_log[256];
    struct context ctx;
    int audio_fd, led_fd;
    pid_t audio_pid, led_pid;
    struct timespec pause = {0, 100000000L};
    int before;
    unsigned int opens_before;
    FILE *config;

    assert(mkdtemp(directory) != NULL);
    snprintf(test_privacy_path, sizeof(test_privacy_path), "%s/privacy", directory);
    snprintf(test_privacy_fallback_path, sizeof(test_privacy_fallback_path), "%s/privacy-fallback", directory);
    snprintf(test_config_path, sizeof(test_config_path), "%s/config.json", directory);
    snprintf(test_lamp_path, sizeof(test_lamp_path), "%s/mute-lamp", directory);
    snprintf(test_status_path, sizeof(test_status_path), "%s/status", directory);
    snprintf(test_status_tmp_path, sizeof(test_status_tmp_path), "%s/status.tmp", directory);
    {
        /* The kernel attribute exists on a device that has the control; the
           fixture has to exist before the daemon tries to write it. */
        FILE *lamp = fopen(test_lamp_path, "w");

        assert(lamp != NULL);
        assert(fclose(lamp) == 0);
    }
    snprintf(audio_socket, sizeof(audio_socket), "%s/audio.sock", directory);
    snprintf(led_socket, sizeof(led_socket), "%s/led.sock", directory);
    snprintf(audio_log, sizeof(audio_log), "%s/audio.log", directory);
    audio_fd = make_listener(audio_socket);
    led_fd = make_listener(led_socket);
    audio_pid = fork();
    assert(audio_pid >= 0);
    if (!audio_pid)
        mock_audio(audio_fd, audio_log);
    led_pid = fork();
    assert(led_pid >= 0);
    if (!led_pid)
        mock_led(led_fd);
    close(audio_fd);
    close(led_fd);

    init_context(&ctx, audio_socket);
    ctx.led_sock = led_socket;
    config = fopen(test_config_path, "w");
    assert(config != NULL);
    fputs("{\"button_tones\":false,\"button_action\":\"playpause\","
          "\"button_action_sounds\":\"custom-chime,listen\","
          "\"button_action_brightness\":23,\"button_mute_brightness\":25}", config);
    assert(fclose(config) == 0);

    refresh_tone_setting(&ctx);
    assert(ctx.tones == 0);
    assert(!strcmp(ctx.action, "playpause"));
    assert(!strcmp(ctx.action_sounds, "custom-chime,listen"));
    assert(ctx.action_brightness == 23 && ctx.mute_brightness == 25);


    /* KEY_MUTE is reported by the kernel transition, not toggled by buttond. */
    write_state(BUTTOND_PRIVACY_STATE_PATH, 0);
    handle_key(&ctx, KEY_MUTE, 1);
    assert(ctx.muted == 0);
    before = log_count(audio_log, "\"cmd\":\"set_mute\"");
    assert(before == 0);

    /* The transition may complete before the event callback. The callback
       synchronizes immediately, so software mute is already asserted. */
    write_state(BUTTOND_PRIVACY_STATE_PATH, 1);
    handle_key(&ctx, KEY_MUTE, 1);
    assert(ctx.muted == 1);
    nanosleep(&pause, NULL);
    assert(log_count(audio_log, "\"cmd\":\"set_mute\"") == before + 1);

    /* The kernel lamp control is driven with the software mute, and the
       capability is reported so the UI can describe the lamp. */
    {
        char lamp[8] = "";
        FILE *file = fopen(test_lamp_path, "r");

        assert(file != NULL);
        assert(fgets(lamp, sizeof(lamp), file) != NULL);
        fclose(file);
        assert(lamp[0] == '1');
        assert(ctx.lamp_supported == 1);
        /* Published as soon as the probe learned it: /buttons reads this file,
           and the heartbeat writes it before the mute indicator runs, so
           waiting for the next cycle served the previous answer for a whole
           cycle -- long enough for both pages to call a lamp the daemon had
           just lit dark. */
        assert(status_field("lamp_control") == 1);
        assert(ctx.lamp_retry_at_ms == 0);
    }
    unlink(test_lamp_path);
    /* Without the attribute the mute still works and the lamp is reported as
       unsupported -- but not as settled: the control belongs to the privacy
       driver, which can bind after this daemon has started, so the probe stays
       alive and the capability recovers without a restart. */
    mute_indicator(&ctx, 0);
    assert(ctx.lamp_supported == 0);
    assert(status_field("lamp_control") == 0);
    assert(ctx.lamp_retry_at_ms != 0);
    assert(!strcmp(ctx.action, "playpause"));
    {
        FILE *lamp = fopen(test_lamp_path, "w");

        assert(lamp != NULL);
        assert(fclose(lamp) == 0);
    }
    mute_indicator(&ctx, 1);
    assert(ctx.lamp_supported == 1);
    assert(status_field("lamp_control") == 1);
    assert(ctx.lamp_retry_at_ms == 0);
    unlink(test_lamp_path);

    /*
     * A write the kernel defers is not an answer to the capability question.
     * The mute lamp's store() returns -EBUSY while the button's latch (or the
     * shutdown dialog) owns the line, and records the request for when the line
     * is free, so calling that "unsupported" would describe a control this image
     * has as missing. A write the kernel refuses outright is the no -- and only a
     * refusal that cannot change (-EOPNOTSUPP: this board's latch owns the line)
     * settles it for good.
     */
    {
        FILE *lamp = fopen(test_lamp_path, "w");

        assert(lamp != NULL);
        assert(fclose(lamp) == 0);
    }
    ctx.lamp_supported = -1;
    write_capability_status(&ctx);   /* the fresh record of an untested control */
    assert(status_field("lamp_control") == -1);
    assert(lamp_failure_is_final(EOPNOTSUPP) && !lamp_failure_is_final(EBUSY) &&
           !lamp_failure_is_final(EIO) && !lamp_failure_is_final(EACCES));
    buttond_fixture_write_errno = EBUSY;
    mute_indicator(&ctx, 1);
    assert(ctx.lamp_supported == -1);
    assert(status_field("lamp_control") == -1);
    /*
     * A deferral cannot unlearn support the kernel has already proven: with a
     * write accepted once, EBUSY leaves both the field and the published record
     * at true. The deferral says nothing about what software can do here, so it
     * must not demote a control this image has.
     */
    ctx.lamp_supported = 1;
    write_capability_status(&ctx);
    assert(status_field("lamp_control") == 1);
    opens_before = buttond_fixture_opens;
    mute_indicator(&ctx, 0);
    assert(buttond_fixture_opens > opens_before);   /* the probe ran, not skipped */
    assert(ctx.lamp_supported == 1);
    assert(status_field("lamp_control") == 1);
    buttond_fixture_write_errno = EIO;
    mute_indicator(&ctx, 0);
    assert(ctx.lamp_supported == 0);
    assert(status_field("lamp_control") == 0);
    assert(ctx.lamp_retry_at_ms != 0);   /* anything but a refusal is retried */
    ctx.lamp_supported = -1;
    buttond_fixture_write_errno = EOPNOTSUPP;
    mute_indicator(&ctx, 1);
    assert(ctx.lamp_supported == 0);
    assert(status_field("lamp_control") == 0);
    assert(ctx.lamp_retry_at_ms == 0);   /* the kernel said no: stop probing */
    /* A settled refusal is not probed again, so the daemon does not even reopen
       the attribute: the line keeps what it had. */
    {
        FILE *lamp = fopen(test_lamp_path, "w");

        assert(lamp != NULL);
        assert(fputs("0", lamp) >= 0);
        assert(fclose(lamp) == 0);
    }
    buttond_fixture_write_errno = 0;
    mute_indicator(&ctx, 1);
    assert(ctx.lamp_supported == 0);
    {
        char lamp[8] = "";
        FILE *file = fopen(test_lamp_path, "r");

        assert(file != NULL);
        assert(fgets(lamp, sizeof(lamp), file) != NULL);
        assert(fclose(file) == 0);
        assert(lamp[0] == '0');
    }
    unlink(test_lamp_path);

    /* A steady asserted latch is idempotent, so the event cannot double-toggle. */
    sync_privacy_state(&ctx);
    nanosleep(&pause, NULL);
    assert(log_count(audio_log, "\"cmd\":\"set_mute\"") == before + 1);

    /* The transition may complete after the event callback. Keep the old
       asserted fixture through the callback, then publish the kernel's new
       state and let the synchronizer observe it. */
    write_state(BUTTOND_PRIVACY_STATE_PATH, 1);
    handle_key(&ctx, KEY_MUTE, 1);
    assert(ctx.muted == 1);
    write_state(BUTTOND_PRIVACY_STATE_PATH, 0);
    sync_privacy_state(&ctx);
    assert(ctx.muted == 0);

    /* Legacy KEY_POWER must not reverse a transition observed before dispatch. */
    write_state(BUTTOND_PRIVACY_STATE_PATH, 1);
    sync_privacy_state(&ctx);
    before = log_count(audio_log, "\"cmd\":\"set_mute\"");
    handle_key(&ctx, KEY_POWER, 1);
    assert(ctx.muted == 1);
    assert(log_count(audio_log, "\"cmd\":\"set_mute\"") == before);
    write_state(BUTTOND_PRIVACY_STATE_PATH, 0);
    sync_privacy_state(&ctx);
    handle_key(&ctx, KEY_POWER, 1);
    assert(ctx.muted == 0);
    /* Also cover completion after KEY_POWER dispatch. */
    handle_key(&ctx, KEY_POWER, 1);
    assert(ctx.muted == 0);
    write_state(BUTTOND_PRIVACY_STATE_PATH, 1);
    sync_privacy_state(&ctx);
    assert(ctx.muted == 1);
    write_state(BUTTOND_PRIVACY_STATE_PATH, 0);
    sync_privacy_state(&ctx);

    /* Unchanged hardware zero never clears a separate API/software mute. */
    ctx.muted = 1;
    sync_privacy_state(&ctx);
    assert(ctx.muted == 1);

    /* Startup privacy asserted forces software mute. */
    ctx.privacy_state_seen = 0;
    ctx.privacy_state = -1;
    ctx.muted = 0;
    write_state(BUTTOND_PRIVACY_STATE_PATH, 1);
    sync_privacy_state(&ctx);
    assert(ctx.muted == 1);

    /* Startup hardware zero preserves an already-muted software state. */
    ctx.privacy_state_seen = 0;
    ctx.privacy_state = -1;
    ctx.muted = 1;
    write_state(BUTTOND_PRIVACY_STATE_PATH, 0);
    sync_privacy_state(&ctx);
    assert(ctx.muted == 1);

    /* Older kernels without the primary path use the documented fallback. */
    unlink(BUTTOND_PRIVACY_STATE_PATH);
    ctx.privacy_state_seen = 0;
    ctx.privacy_state = -1;
    ctx.muted = 0;
    write_state(BUTTOND_PRIVACY_STATE_FALLBACK_PATH, 1);
    sync_privacy_state(&ctx);
    assert(ctx.muted == 1);

    /*
     * A latch transition is published to the UI even when audiod cannot be told:
     * the reported lamp state must not depend on the software sync succeeding.
     */
    {
        char dead_socket[256];

        snprintf(dead_socket, sizeof(dead_socket), "%s/absent-audio.sock",
                 directory);
        unlink(BUTTOND_PRIVACY_STATE_FALLBACK_PATH);
        ctx.privacy_state_seen = 1;
        ctx.privacy_state = 0;
        ctx.privacy_observed = 0;
        ctx.muted = 0;
        write_state(BUTTOND_PRIVACY_STATE_PATH, 1);
        ctx.audio_sock = dead_socket;
        sync_privacy_state(&ctx);
        assert(ctx.privacy_observed == 1);
        assert(ctx.muted == 0);
        assert(ctx.privacy_state == 0);
        ctx.audio_sock = audio_socket;
    }

    /* The transition itself is still retried while steady. */
    sync_privacy_state(&ctx);
    assert(ctx.muted == 1 && ctx.privacy_state == 1);

    /* An unreadable latch publishes as unknown, not as the last value. */
    unlink(BUTTOND_PRIVACY_STATE_PATH);
    sync_privacy_state(&ctx);
    assert(ctx.privacy_observed == -1);
    assert(ctx.privacy_state == 1);   /* the synchronization anchor is untouched */

    /* And it recovers as soon as the latch can be read again. */
    write_state(BUTTOND_PRIVACY_STATE_PATH, 1);
    sync_privacy_state(&ctx);
    assert(ctx.privacy_observed == 1);

    assert(buttond_fixture_opens > 0);
    kill(audio_pid, SIGTERM);
    kill(led_pid, SIGTERM);
    waitpid(audio_pid, NULL, 0);
    waitpid(led_pid, NULL, 0);
    unlink(audio_socket);
    unlink(led_socket);
    unlink(audio_log);
    unlink(BUTTOND_PRIVACY_STATE_PATH);
    unlink(BUTTOND_PRIVACY_STATE_FALLBACK_PATH);
    unlink(test_config_path);
    unlink(test_status_path);
    unlink(test_status_tmp_path);
    unlink(test_lamp_path);
    rmdir(directory);
    puts("buttond privacy synchronization: startup, transitions, fallback, and no double-toggle: ok");
    return 0;
}
