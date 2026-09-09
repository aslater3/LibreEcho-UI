#define _POSIX_C_SOURCE 200809L

/* Host regression for the kernel-owned privacy latch synchronizer. */
#define BUTTOND_PRIVACY_STATE_PATH "/tmp/libreecho-buttond-privacy-state-test"
#define BUTTOND_PRIVACY_STATE_FALLBACK_PATH "/tmp/libreecho-buttond-privacy-state-fallback-test"
#define main buttond_program_main
#include "../src/adapter/buttond.c"
#undef main

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

    unlink(BUTTOND_PRIVACY_STATE_PATH);
    unlink(BUTTOND_PRIVACY_STATE_FALLBACK_PATH);
    assert(mkdtemp(directory) != NULL);
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

    kill(audio_pid, SIGTERM);
    kill(led_pid, SIGTERM);
    waitpid(audio_pid, NULL, 0);
    waitpid(led_pid, NULL, 0);
    unlink(audio_socket);
    unlink(led_socket);
    unlink(audio_log);
    unlink(BUTTOND_PRIVACY_STATE_PATH);
    unlink(BUTTOND_PRIVACY_STATE_FALLBACK_PATH);
    rmdir(directory);
    puts("buttond privacy synchronization: startup, transitions, fallback, and no double-toggle: ok");
    return 0;
}
