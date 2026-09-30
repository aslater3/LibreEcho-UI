#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#define main airplayd_program_main
#include "../src/adapter/airplayd.c"
#undef main

/* Link the real controller poller and audiod request handler in one host
 * fixture. Only the hardware codec ioctl is replaced. */
static char integration_root[] = "/tmp/libreecho-airplay-audiod-XXXXXX";
static char integration_active_path[128], integration_restore_path[128];
static char integration_ack_path[128], integration_master_path[128];
#define LE_AIRPLAY_ACTIVE_PATH integration_active_path
#define LE_AIRPLAY_RESTORE_PATH integration_restore_path
#define LE_AIRPLAY_MASTER_ACK_PATH integration_ack_path
#define LE_MASTER_VOLUME_DIR integration_root
#define LE_MASTER_VOLUME_PATH integration_master_path
#include <sys/syscall.h>
#include <stdarg.h>
static int integration_ioctl(int fd, unsigned long request, ...);
#define ioctl integration_ioctl
#define json_bool audiod_json_bool
#define main audiod_program_main
#include "../src/adapter/audiod.c"
#undef main
#undef json_bool
#undef ioctl

#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>

static int integration_codec_raw, integration_codec_writes;
static int integration_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    void *value;
    va_start(args, request);
    value = va_arg(args, void *);
    va_end(args);
    if (fd != 100000) return (int)syscall(SYS_ioctl, fd, request, value);
    if (request == SNDRV_CTL_IOCTL_ELEM_READ) {
        ((struct snd_ctl_elem_value *)value)->value.integer.value[0] = integration_codec_raw;
        return 0;
    }
    if (request == SNDRV_CTL_IOCTL_ELEM_WRITE) {
        integration_codec_raw = (int)((struct snd_ctl_elem_value *)value)->value.integer.value[0];
        ++integration_codec_writes;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

/* The shared supervisor is an external dependency. This fake answers one
 * readiness probe at a time and lets the tests assert both the ready and the
 * degraded answer without any Avahi or D-Bus binary on the host. */
static pid_t spawn_status_supervisor(const char *path, const char *status)
{
    pid_t pid = fork();
    struct timespec delay = { 0, 50000000L };
    struct sockaddr_un address;
    char message[16];
    int fd;

    if (pid != 0) {
        nanosleep(&delay, NULL);
        return pid;
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    unlink(path);
    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 1) < 0)
        _exit(1);
    for (;;) {
        int client = accept(fd, NULL, NULL);
        if (client < 0)
            _exit(1);
        if (read(client, message, sizeof(message)) > 0)
            (void)write(client, status, strlen(status));
        close(client);
    }
}

static void test_sender_master_session_hooks(void)
{
    FILE *file = fopen("config/airplay2.conf", "r");
    char config[2048];
    size_t n;
    assert(file);
    n = fread(config, 1, sizeof(config) - 1, file);
    assert(!ferror(file));
    assert(feof(file));
    fclose(file);
    config[n] = '\0';
    assert(strstr(config, "run_this_before_play_begins = \"/usr/local/sbin/libreecho-airplay-audio --start\""));
    assert(strstr(config, "run_this_after_play_ends = \"/usr/local/sbin/libreecho-airplay-audio --stop\""));
    assert(strstr(config, "wait_for_completion = \"yes\""));
    assert(strstr(config, "ignore_volume_control = \"yes\""));
}

static void test_sender_master_mapping(void)
{
    int value;
    assert(airplay_db_to_percent("-30\n", &value) == 0 && value == 1);
    assert(airplay_db_to_percent("-15\n", &value) == 0 && value == 51);
    assert(airplay_db_to_percent("0\n", &value) == 0 && value == 100);
    assert(airplay_db_to_percent("-144\n", &value) == 0 && value == 0);
    assert(airplay_db_to_percent("-31\n", &value) < 0);
    assert(airplay_db_to_percent("nan\n", &value) < 0);
    assert(airplay_db_to_percent("-145\n", &value) < 0);
    assert(airplay_db_to_percent("1\n", &value) < 0);
    assert(airplay_db_to_percent("-20oops", &value) < 0);
}

static void init_ctx(struct airplay_ctx *ctx);

static void test_sender_master_poll(void)
{
    struct airplay_ctx ctx;
    char root[] = "/tmp/libreecho-master-test-XXXXXX";
    char marker[256], volume[256], ack[256], temp[260], victim[256], socket_path[128];
    int fd, count[2];
    pid_t server;
    struct stat m, v;
    char sent[16] = {0};
    assert(mkdtemp(root));
    init_ctx(&ctx);
    ctx.enabled = 1;
    snprintf(ctx.volume_root, sizeof(ctx.volume_root), "%s", root);
    snprintf(marker, sizeof(marker), "%s/airplay.active", root);
    snprintf(volume, sizeof(volume), "%s/airplay.volume", root);
    snprintf(ack, sizeof(ack), "%s/airplay.master", root);
    snprintf(temp, sizeof(temp), "%s.tmp", ack);
    snprintf(victim, sizeof(victim), "%s/victim", root);
    fd = open(victim, O_CREAT | O_WRONLY, 0600); assert(fd >= 0);
    assert(write(fd, "safe", 4) == 4); close(fd);
    assert(symlink(victim, temp) == 0);
    snprintf(socket_path, sizeof(socket_path), "%s/audio.sock", root);
    snprintf(ctx.master_socket, sizeof(ctx.master_socket), "%s", socket_path);
    assert(pipe(count) == 0);
    fd = open(marker, O_CREAT | O_WRONLY, 0600); assert(fd >= 0); close(fd);
    fd = open(volume, O_CREAT | O_WRONLY, 0600); assert(fd >= 0);
    assert(write(fd, "-15\n", 4) == 4); close(fd);
    airplay_master_poll(&ctx);
    assert(!ctx.applied && access(ack, F_OK) != 0);
    server = fork(); assert(server >= 0);
    if (!server) {
        int listener = le_adapter_listen(socket_path);
        int client;
        char input[512], response[128];
        alarm(5);
        close(count[0]);
        if (listener < 0) _exit(1);
        client = le_adapter_accept(listener);
        if (client < 0) _exit(2);
        for (int i = 1; i <= 2; ++i) {
            ssize_t length = read(client, input, sizeof(input) - 1);
            if (length <= 0) _exit(3);
            input[length] = '\0';
            if (i == 1 && (!strstr(input, "\"cmd\":\"airplay_volume\"") ||
                           !strstr(input, "\"volume\":51"))) _exit(4);
            /* A button wins between the successful write and status read.
             * Do not replay the sender's old callback on a later poll. */
            int n = snprintf(response, sizeof(response),
                             "{\"v\":1,\"id\":%d,\"ok\":true,\"data\":{\"volume\":%d}}\n",
                             i, i == 1 ? 51 : 22);
            if (write(client, response, (size_t)n) != n) _exit(5);
            write(count[1], "x", 1);
        }
        close(client);
        client = le_adapter_accept(listener);
        if (client < 0) _exit(6);
        for (int i = 1; i <= 2; ++i) {
            ssize_t length = read(client, input, sizeof(input) - 1);
            if (length <= 0) _exit(7);
            input[length] = '\0';
            if (!strstr(input, i == 1 ? "\"cmd\":\"airplay_volume\"" : "\"cmd\":\"status\"")) _exit(8);
            int n = snprintf(response, sizeof(response),
                             "{\"v\":1,\"id\":%d,\"ok\":true,\"data\":{\"volume\":22}}\n", i);
            if (write(client, response, (size_t)n) != n) _exit(9);
            write(count[1], "r", 1);
        }
        close(client);
        client = le_adapter_accept(listener);
        if (client < 0) _exit(10);
        ssize_t length = read(client, input, sizeof(input) - 1);
        if (length <= 0) _exit(11);
        input[length] = '\0';
        if (!strstr(input, "\"cmd\":\"airplay_end\"") || !strstr(input, "\"session\":")) _exit(12);
        int n = snprintf(response, sizeof(response),
                         "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{}}\n");
        if (write(client, response, (size_t)n) != n) _exit(13);
        write(count[1], "e", 1);
        close(client); close(listener); _exit(0);
    }
    close(count[1]);
    for (int i = 0; i < 100 && access(socket_path, F_OK) != 0; ++i) usleep(1000);
    airplay_master_poll(&ctx);
    fd = open(victim, O_RDONLY); assert(fd >= 0);
    assert(read(fd, sent, sizeof(sent)) == 4 && !memcmp(sent, "safe", 4)); close(fd);
    assert(stat(marker, &m) == 0 && stat(volume, &v) == 0 && access(ack, F_OK) == 0);
    assert(ctx.applied && ctx.applied_marker_ino == m.st_ino && ctx.applied_volume_ino == v.st_ino);
    /* An owner that briefly loses the controller revokes admission. The
     * cached callback must no longer count as acknowledged. */
    assert(unlink(ack) == 0);
    snprintf(ctx.master_socket, sizeof(ctx.master_socket), "%s/missing.sock", root);
    airplay_master_poll(&ctx);
    assert(!ctx.applied && !ctx.write_confirmed);
    snprintf(ctx.master_socket, sizeof(ctx.master_socket), "%s", socket_path);
    airplay_master_poll(&ctx);
    assert(ctx.applied && ctx.write_confirmed && access(ack, F_OK) == 0);
    /* A symlinked callback is not a sender-owned regular file, even when it
     * contains a valid decibel value. Keep the admitted session untouched. */
    assert(unlink(volume) == 0);
    assert(unlink(temp) == 0);
    fd = open(temp, O_CREAT | O_WRONLY | O_TRUNC, 0600); assert(fd >= 0);
    assert(write(fd, "-30\n", 4) == 4); close(fd);
    assert(symlink(temp, volume) == 0);
    airplay_master_poll(&ctx);
    assert(ctx.applied && ctx.applied_volume_ino == v.st_ino);
    assert(unlink(marker) == 0);
    airplay_master_poll(&ctx);
    assert(!ctx.master_session[0]);
    airplay_master_poll(&ctx);
    /* The stub server signals each exchange after replying, so the client can
     * finish first. Reap the server before counting to avoid racing it. */
    int child_status;
    assert(waitpid(server, &child_status, 0) == server &&
           WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    assert(read(count[0], sent, sizeof(sent)) == 5);
    close(count[0]); unlink(temp); unlink(victim); unlink(ack); unlink(volume); unlink(marker); unlink(socket_path); rmdir(root);
}

/* A status timeout after a confirmed write must never replay that write. */
static void test_master_status_timeout_preserves_button_change(void)
{
    struct airplay_ctx ctx;
    char root[] = "/tmp/libreecho-master-retry-XXXXXX";
    char marker[256], volume[256], ack[256], socket_path[128];
    char calls[16] = {0};
    int fd, events[2], status;
    pid_t server;
    assert(mkdtemp(root));
    init_ctx(&ctx);
    ctx.enabled = 1;
    snprintf(ctx.volume_root, sizeof(ctx.volume_root), "%s", root);
    snprintf(marker, sizeof(marker), "%s/airplay.active", root);
    snprintf(volume, sizeof(volume), "%s/airplay.volume", root);
    snprintf(ack, sizeof(ack), "%s/airplay.master", root);
    snprintf(socket_path, sizeof(socket_path), "%s/audio.sock", root);
    snprintf(ctx.master_socket, sizeof(ctx.master_socket), "%s", socket_path);
    fd = open(marker, O_CREAT | O_WRONLY, 0600); assert(fd >= 0); close(fd);
    fd = open(volume, O_CREAT | O_WRONLY, 0600); assert(fd >= 0);
    assert(write(fd, "-15\n", 4) == 4); close(fd);
    assert(pipe(events) == 0);
    server = fork(); assert(server >= 0);
    if (!server) {
        int listener = le_adapter_listen(socket_path);
        struct timespec delay = { 0, 150000000L };
        close(events[0]);
        if (listener < 0) _exit(1);
        for (int connection = 0; connection < 3; ++connection) {
            int client = le_adapter_accept(listener);
            if (client < 0) _exit(2);
            for (;;) {
                char input[512], response[128], event;
                ssize_t length = read(client, input, sizeof(input) - 1);
                int id, n;
                if (length == 0) break;
                if (length < 0) _exit(3);
                input[length] = '\0';
                event = strstr(input, "\"cmd\":\"airplay_volume\"") ? 'W' :
                        strstr(input, "\"cmd\":\"status\"") ? 'S' : '?';
                if (write(events[1], &event, 1) != 1 || event == '?') _exit(4);
                if (event == 'W' && !strstr(input, connection == 2 ?
                                           "\"volume\":1" : "\"volume\":51")) _exit(7);
                if (connection == 0 && event == 'S') {
                    nanosleep(&delay, NULL); /* exceed the 100 ms IPC timeout */
                    break;
                }
                if (!strstr(input, "\"id\":") ||
                    sscanf(strstr(input, "\"id\":"), "\"id\":%d", &id) != 1)
                    _exit(5);
                n = snprintf(response, sizeof(response),
                             "{\"v\":1,\"id\":%d,\"ok\":true,\"data\":{\"volume\":22}}\n", id);
                if (write(client, response, (size_t)n) != n) _exit(6);
            }
            close(client);
        }
        close(listener); close(events[1]); _exit(0);
    }
    close(events[1]);
    for (int i = 0; i < 100 && access(socket_path, F_OK) != 0; ++i) usleep(1000);
    airplay_master_poll(&ctx);
    assert(!ctx.applied && access(ack, F_OK) != 0);
    /* A button has now changed the master to 22, before status recovery. */
    airplay_master_poll(&ctx);
    assert(ctx.applied && access(ack, F_OK) == 0);
    assert(unlink(volume) == 0);
    fd = open(volume, O_CREAT | O_WRONLY, 0600); assert(fd >= 0);
    assert(write(fd, "-30\n", 4) == 4); close(fd);
    airplay_master_poll(&ctx);
    assert(waitpid(server, &status, 0) == server && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(read(events[0], calls, sizeof(calls)) == 5);
    assert(!memcmp(calls, "WSSWS", 5));
    close(events[0]); unlink(ack); unlink(volume); unlink(marker); unlink(socket_path); rmdir(root);
}

static void test_master_write_timeout_still_ends(void)
{
    struct airplay_ctx ctx;
    char root[] = "/tmp/libreecho-master-lost-XXXXXX";
    char marker[256], volume[256], socket_path[128], calls[4] = {0};
    int fd, events[2], status;
    pid_t server;
    assert(mkdtemp(root));
    init_ctx(&ctx);
    ctx.enabled = 1;
    snprintf(ctx.volume_root, sizeof(ctx.volume_root), "%s", root);
    snprintf(marker, sizeof(marker), "%s/airplay.active", root);
    snprintf(volume, sizeof(volume), "%s/airplay.volume", root);
    snprintf(socket_path, sizeof(socket_path), "%s/audio.sock", root);
    snprintf(ctx.master_socket, sizeof(ctx.master_socket), "%s", socket_path);
    fd = open(marker, O_CREAT | O_WRONLY, 0600); assert(fd >= 0); close(fd);
    fd = open(volume, O_CREAT | O_WRONLY, 0600); assert(fd >= 0);
    assert(write(fd, "-144\n", 5) == 5); close(fd);
    assert(pipe(events) == 0);
    server = fork(); assert(server >= 0);
    if (!server) {
        int listener = le_adapter_listen(socket_path);
        struct timespec delay = { 0, 150000000L };
        close(events[0]);
        if (listener < 0) _exit(1);
        for (int i = 0; i < 2; ++i) {
            int client = le_adapter_accept(listener);
            char input[512], response[128], event;
            ssize_t length;
            int id, n;
            if (client < 0) _exit(2);
            length = read(client, input, sizeof(input) - 1);
            if (length <= 0) _exit(3);
            input[length] = '\0';
            event = strstr(input, i == 0 ? "\"cmd\":\"airplay_volume\"" :
                                      "\"cmd\":\"airplay_end\"") ? (i == 0 ? 'W' : 'E') : '?';
            if (event == '?' || (i == 0 && !strstr(input, "\"callback\":\"")) ||
                write(events[1], &event, 1) != 1) _exit(4);
            if (i == 0) { nanosleep(&delay, NULL); close(client); continue; }
            if (!strstr(input, "\"id\":") ||
                sscanf(strstr(input, "\"id\":"), "\"id\":%d", &id) != 1) _exit(5);
            n = snprintf(response, sizeof(response),
                         "{\"v\":1,\"id\":%d,\"ok\":true,\"data\":{}}\n", id);
            if (write(client, response, (size_t)n) != n) _exit(6);
            close(client);
        }
        close(listener); close(events[1]); _exit(0);
    }
    close(events[1]);
    for (int i = 0; i < 100 && access(socket_path, F_OK) != 0; ++i) usleep(1000);
    airplay_master_poll(&ctx);
    assert(!ctx.write_confirmed && ctx.master_session[0]);
    assert(unlink(marker) == 0);
    airplay_master_poll(&ctx);
    assert(!ctx.master_session[0]);
    assert(waitpid(server, &status, 0) == server && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(read(events[0], calls, sizeof(calls)) == 2 && !memcmp(calls, "WE", 2));
    close(events[0]); unlink(volume); unlink(socket_path); rmdir(root);
}

/* A rejected audiod write must not publish a sender-media acknowledgment. */
static void test_master_rejected_write_stays_gated(void)
{
    struct airplay_ctx ctx;
    char root[] = "/tmp/libreecho-master-rejected-XXXXXX";
    char marker[256], volume[256], ack[256], socket_path[128];
    int fd, status;
    pid_t server;
    assert(mkdtemp(root));
    init_ctx(&ctx);
    ctx.enabled = 1;
    snprintf(ctx.volume_root, sizeof(ctx.volume_root), "%s", root);
    snprintf(marker, sizeof(marker), "%s/airplay.active", root);
    snprintf(volume, sizeof(volume), "%s/airplay.volume", root);
    snprintf(ack, sizeof(ack), "%s/airplay.master", root);
    snprintf(socket_path, sizeof(socket_path), "%s/audio.sock", root);
    snprintf(ctx.master_socket, sizeof(ctx.master_socket), "%s", socket_path);
    fd = open(marker, O_CREAT | O_WRONLY, 0600); assert(fd >= 0); close(fd);
    fd = open(volume, O_CREAT | O_WRONLY, 0600); assert(fd >= 0);
    assert(write(fd, "-144\n", 5) == 5); close(fd);
    server = fork(); assert(server >= 0);
    if (!server) {
        int listener = le_adapter_listen(socket_path);
        if (listener < 0) _exit(1);
        for (int i = 0; i < 2; ++i) {
            int client = le_adapter_accept(listener);
            char input[512], response[128];
            int id, n;
            ssize_t length;
            if (client < 0) _exit(2);
            length = read(client, input, sizeof(input) - 1);
            if (length <= 0) _exit(3);
            input[length] = '\0';
            if (!strstr(input, "\"cmd\":\"airplay_volume\"") ||
                !strstr(input, "\"id\":") ||
                sscanf(strstr(input, "\"id\":"), "\"id\":%d", &id) != 1) _exit(4);
            n = snprintf(response, sizeof(response),
                         "{\"v\":1,\"id\":%d,\"ok\":false,\"error\":\"ended AirPlay session\"}\n", id);
            if (write(client, response, (size_t)n) != n) _exit(5);
            close(client);
        }
        close(listener); _exit(0);
    }
    for (int i = 0; i < 100 && access(socket_path, F_OK) != 0; ++i) usleep(1000);
    airplay_master_poll(&ctx);
    assert(!ctx.write_confirmed && !ctx.applied && access(ack, F_OK) != 0);
    airplay_master_poll(&ctx);
    assert(!ctx.write_confirmed && !ctx.applied && access(ack, F_OK) != 0);
    assert(waitpid(server, &status, 0) == server && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    unlink(marker); unlink(volume); unlink(socket_path); rmdir(root);
}

static void init_ctx(struct airplay_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->listener = -1;
    ctx->metadata_fd = -1;
    ctx->nqptp_pid = -1;
    ctx->audio_pid = -1;
    ctx->engine_pid = -1;
    ctx->shairport_pid = -1;
    snprintf(ctx->mdns_socket, sizeof(ctx->mdns_socket), "%s",
             "/tmp/libreecho-airplay-mdns-test.sock");
    unlink(ctx->mdns_socket);
}

/* One live audiod state machine serves both sessions over a surviving Unix
 * listener. The controller makes real adapter calls; the server dispatches
 * every wire request through audiod's unmodified handle_request(). */
static void integration_server(int listener, int done, int events)
{
    struct audio_hw audio;
    int ends = 0, dropped = 0;
    memset(&audio, 0, sizeof(audio));
    audio.ctl_fd = 100000;
    audio.master.found = audio.master.writable = 1;
    audio.master.info.type = SNDRV_CTL_ELEM_TYPE_INTEGER;
    audio.master.info.value.integer.min = 0;
    audio.master.info.value.integer.max = 151;
    audio.master.info.count = 1;
    snprintf(audio.master.name, sizeof(audio.master.name), "PCM Playback Volume");
    audio.volume = audio.requested_volume = 42;
    integration_codec_raw = 0;
    assert(audio_set_volume(&audio, 42) == 0);
    for (;;) {
        struct pollfd fds[2] = {{listener, POLLIN, 0}, {done, POLLIN | POLLHUP, 0}};
        assert(poll(fds, 2, 3000) > 0);
        if (fds[1].revents) break;
        if (fds[0].revents & POLLIN) {
            int client = le_adapter_accept(listener);
            char request[LE_ADAPTER_MSG_MAX], reply[LE_ADAPTER_MSG_MAX];
            assert(client >= 0);
            for (;;) {
                ssize_t n = read(client, request, sizeof(request) - 1);
                char event;
                if (!n) break;
                assert(n > 0);
                request[n] = '\0';
                event = strstr(request, "\"cmd\":\"airplay_volume\"") ? 'W' :
                        strstr(request, "\"cmd\":\"airplay_end\"") ? 'E' :
                        strstr(request, "\"cmd\":\"set_volume\"") ? 'B' : 'S';
                n = handle_request(&audio, request, reply, sizeof(reply));
                assert(n > 0);
                if (event == 'W' && !dropped) {
                    dropped = 1; /* apply A, lose its wire reply */
                } else {
                    assert(write(client, reply, (size_t)n) == n);
                }
                assert(write(events, &event, 1) == 1);
                if (event == 'E' && ++ends == 1) {
                    /* Recreate audiod from its runtime files before B, while
                     * the socket listener and metadata FIFO both survive. */
                    struct audio_hw restarted;
                    memset(&restarted, 0, sizeof(restarted));
                    restarted.ctl_fd = 100000;
                    restarted.master.found = restarted.master.writable = 1;
                    restarted.master.info.type = SNDRV_CTL_ELEM_TYPE_INTEGER;
                    restarted.master.info.value.integer.min = 0;
                    restarted.master.info.value.integer.max = 151;
                    restarted.master.info.count = 1;
                    snprintf(restarted.master.name, sizeof(restarted.master.name),
                             "PCM Playback Volume");
                    restarted.volume = restarted.requested_volume = 50;
                    restarted.have_card_info = 1;
                    apply_persisted_levels(&restarted);
                    airplay_restore_load(&restarted);
                    assert(restarted.volume == 27 && restarted.airplay_newest_ended);
                    audio = restarted;
                }
            }
            close(client);
        }
    }
    assert(dropped && ends == 2 && integration_codec_writes == 1 && integration_codec_raw == 127);
    close(events); close(done); close(listener);
}

static void integration_call(const char *socket_path, const char *command,
                             const char *args, int expected_ok, int expected_volume)
{
    struct le_adapter *adapter = le_adapter_connect(socket_path, 100);
    char response[1024], expected[32];
    int result;
    assert(adapter);
    le_adapter_set_io_timeout(adapter, 100);
    result = le_adapter_call(adapter, command, args, response, sizeof(response));
    le_adapter_close(adapter);
    if (expected_ok) assert(result == LE_ADAPTER_OK);
    else assert(result != LE_ADAPTER_OK);
    if (expected_volume >= 0) {
        snprintf(expected, sizeof(expected), "\"volume\":%d", expected_volume);
        assert(strstr(response, expected));
    }
}

static void integration_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(close(fd) == 0);
}

static void integration_master_is(int volume)
{
    char text[16], expected[16];
    int fd = open(LE_MASTER_VOLUME_PATH, O_RDONLY | O_NOFOLLOW);
    ssize_t n;
    assert(fd >= 0);
    n = read(fd, text, sizeof(text) - 1);
    assert(n > 0);
    close(fd);
    text[n] = '\0';
    snprintf(expected, sizeof(expected), "%d\n", volume);
    assert(!strcmp(text, expected));
}

static void integration_ack_matches(const char *volume_path)
{
    struct stat m, v;
    char observed[192], expected[192];
    int fd = open(LE_AIRPLAY_MASTER_ACK_PATH, O_RDONLY | O_NOFOLLOW);
    ssize_t n;
    assert(fd >= 0);
    n = read(fd, observed, sizeof(observed) - 1);
    assert(n > 0);
    close(fd);
    observed[n] = '\0';
    assert(stat(LE_AIRPLAY_ACTIVE_PATH, &m) == 0 && stat(volume_path, &v) == 0);
    snprintf(expected, sizeof(expected), "%llu %llu %lld %ld %llu %llu %lld %ld\n",
             (unsigned long long)m.st_dev, (unsigned long long)m.st_ino,
             (long long)m.st_ctim.tv_sec, m.st_ctim.tv_nsec,
             (unsigned long long)v.st_dev, (unsigned long long)v.st_ino,
             (long long)v.st_ctim.tv_sec, v.st_ctim.tv_nsec);
    assert(!strcmp(observed, expected));
}

static void test_two_sessions_real_audiod_transport(void)
{
    static const char begin[] = "<item><type>ssnc</type><code>pbeg</code><length>0</length></item>";
    static const char end[] = "<item><type>ssnc</type><code>pend</code><length>0</length></item>";
    struct airplay_ctx ctx;
    char socket_path[128], volume_path[128], ack_path[128], old_session[96], old_callback[96];
    char args[256], event_log[32] = {0};
    int listener, done[2], events[2], status;
    pid_t server;
    struct stat marker, callback;
    struct timespec delay = {0, 2000000};
    assert(mkdtemp(integration_root));
    assert(snprintf(integration_active_path, sizeof(integration_active_path),
                    "%s/airplay.active", integration_root) < (int)sizeof(integration_active_path));
    assert(snprintf(integration_restore_path, sizeof(integration_restore_path),
                    "%s/airplay.restore", integration_root) < (int)sizeof(integration_restore_path));
    assert(snprintf(integration_ack_path, sizeof(integration_ack_path),
                    "%s/airplay.master", integration_root) < (int)sizeof(integration_ack_path));
    assert(snprintf(integration_master_path, sizeof(integration_master_path),
                    "%s/master.volume", integration_root) < (int)sizeof(integration_master_path));
    init_ctx(&ctx);
    ctx.enabled = 1;
    snprintf(ctx.volume_root, sizeof(ctx.volume_root), "%s", LE_MASTER_VOLUME_DIR);
    snprintf(socket_path, sizeof(socket_path), "%s/audio.sock", LE_MASTER_VOLUME_DIR);
    snprintf(ctx.master_socket, sizeof(ctx.master_socket), "%s", socket_path);
    snprintf(ctx.metadata_path, sizeof(ctx.metadata_path), "%s/metadata", LE_MASTER_VOLUME_DIR);
    snprintf(volume_path, sizeof(volume_path), "%s/airplay.volume", LE_MASTER_VOLUME_DIR);
    snprintf(ack_path, sizeof(ack_path), "%s", LE_AIRPLAY_MASTER_ACK_PATH);
    assert(metadata_fifo_open(&ctx) == 0);
    listener = le_adapter_listen(socket_path);
    assert(listener >= 0 && pipe(done) == 0 && pipe(events) == 0);
    server = fork(); assert(server >= 0);
    if (!server) {
        close(done[1]); close(events[0]);
        integration_server(listener, done[0], events[1]);
        _exit(0);
    }
    close(done[0]); close(events[1]); close(listener);
    integration_file(LE_AIRPLAY_ACTIVE_PATH, "");
    integration_file(volume_path, "-144\n");
    assert(stat(LE_AIRPLAY_ACTIVE_PATH, &marker) == 0 && stat(volume_path, &callback) == 0);
    snprintf(old_session, sizeof(old_session), "%llu:%llu:%lld:%ld",
             (unsigned long long)marker.st_dev, (unsigned long long)marker.st_ino,
             (long long)marker.st_ctim.tv_sec, marker.st_ctim.tv_nsec);
    snprintf(old_callback, sizeof(old_callback), "%llu:%llu:%lld:%ld",
             (unsigned long long)callback.st_dev, (unsigned long long)callback.st_ino,
             (long long)callback.st_ctim.tv_sec, callback.st_ctim.tv_nsec);
    assert(write(ctx.metadata_fd, begin, sizeof(begin) - 1) == (ssize_t)(sizeof(begin) - 1));
    metadata_fifo_drain(&ctx);
    assert(ctx.playing);
    airplay_master_poll(&ctx);
    assert(!ctx.applied && access(ack_path, F_OK) != 0);
    integration_master_is(0); /* audiod applied A, but its reply was lost */
    airplay_master_poll(&ctx);
    assert(ctx.applied && !strcmp(ctx.master_session, old_session));
    integration_master_is(0);
    integration_ack_matches(volume_path);
    integration_call(socket_path, "status", "{}", 1, 0);
    airplay_master_poll(&ctx); /* no duplicate sender write */
    integration_call(socket_path, "set_volume", "{\"volume\":27}", 1, -1);
    integration_master_is(27);
    integration_call(socket_path, "status", "{}", 1, 27);
    assert(write(ctx.metadata_fd, end, sizeof(end) - 1) == (ssize_t)(sizeof(end) - 1));
    metadata_fifo_drain(&ctx);
    assert(!ctx.playing && ctx.metadata_fd >= 0);
    assert(unlink(LE_AIRPLAY_ACTIVE_PATH) == 0);
    airplay_master_poll(&ctx);
    assert(!ctx.master_session[0] && access(ack_path, F_OK) != 0);
    integration_master_is(27);
    integration_call(socket_path, "status", "{}", 1, 27);
    airplay_master_poll(&ctx); /* one restoration only */
    nanosleep(&delay, NULL);
    integration_file(LE_AIRPLAY_ACTIVE_PATH, "");
    assert(unlink(volume_path) == 0);
    integration_file(volume_path, "-15\n");
    assert(write(ctx.metadata_fd, begin, sizeof(begin) - 1) == (ssize_t)(sizeof(begin) - 1));
    metadata_fifo_drain(&ctx);
    airplay_master_poll(&ctx);
    assert(ctx.applied && ctx.playing && strcmp(ctx.master_session, old_session));
    integration_master_is(51);
    integration_ack_matches(volume_path);
    integration_call(socket_path, "status", "{}", 1, 51);
    snprintf(args, sizeof(args), "{\"session\":\"%s\",\"callback\":\"%s\",\"volume\":0}",
             old_session, old_callback);
    integration_call(socket_path, "airplay_volume", args, 0, -1);
    integration_master_is(51);
    integration_call(socket_path, "status", "{}", 1, 51);
    assert(write(ctx.metadata_fd, end, sizeof(end) - 1) == (ssize_t)(sizeof(end) - 1));
    metadata_fifo_drain(&ctx);
    assert(unlink(LE_AIRPLAY_ACTIVE_PATH) == 0);
    airplay_master_poll(&ctx);
    integration_master_is(27);
    integration_call(socket_path, "status", "{}", 1, 27);
    assert(!ctx.master_session[0] && access(ack_path, F_OK) != 0);
    airplay_master_poll(&ctx);
    close(done[1]);
    assert(waitpid(server, &status, 0) == server && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(read(events[0], event_log, sizeof(event_log)) == 15);
    assert(!memcmp(event_log, "WWSSBSESWSSWSES", 15));
    close(events[0]);
    metadata_fifo_close(&ctx);
    assert(unlink(ctx.metadata_path) == 0);
    assert(unlink(volume_path) == 0);
    assert(unlink(LE_AIRPLAY_RESTORE_PATH) == 0);
    assert(unlink(LE_MASTER_VOLUME_PATH) == 0);
    assert(unlink(socket_path) == 0);
    assert(rmdir(LE_MASTER_VOLUME_DIR) == 0);
}

static void feed_fragmented(struct airplay_ctx *ctx, const char *text,
                            size_t fragment)
{
    size_t length = strlen(text);
    size_t offset = 0;

    while (offset < length) {
        size_t count = length - offset;
        if (count > fragment)
            count = fragment;
        metadata_parser_feed(ctx, text + offset, count);
        offset += count;
    }
}

static void append_status(const struct airplay_ctx *ctx,
                          char *buffer, size_t size)
{
    size_t used = 0;

    buffer[0] = '\0';
    assert(json_append_raw(buffer, size, &used, "{\"base\":true") == 0);
    assert(metadata_append_status(ctx, buffer, size, &used) == 0);
    assert(json_append_raw(buffer, size, &used, "}") == 0);
}

static void test_fragmented_base64_and_json(void)
{
    static const char begin[] =
        "<item><type>73736e63</type><code>70626567</code>"
        "<length>0</length></item>";
    static const char title[] =
        "<item><type>636f7265</type><code>6d696e6d</code>"
        "<length>17</length><data encoding=\"base64\">"
        "TGlicmVFY2hvICJMaXZlIgo=</data></item>";
    static const char artist[] =
        "<item><type>core</type><code>asar</code>"
        "<length>11</length><data encoding='base64'>"
        "VGVzdCBBcnRpc3Q=</data></item>";
    static const char album[] =
        "<item><type>636f7265</type><code>6173616c</code>"
        "<length>9</length><data encoding=\"base64\">\n"
        "VGVzdCBEaXNj\n</data></item>";
    struct airplay_ctx ctx;
    char status[2048];

    init_ctx(&ctx);
    feed_fragmented(&ctx, begin, 1);
    feed_fragmented(&ctx, title, 2);
    feed_fragmented(&ctx, artist, 3);
    feed_fragmented(&ctx, album, 5);
    assert(ctx.playing == 1);
    assert(!strcmp(ctx.title, "LibreEcho \"Live\"\n"));
    assert(!strcmp(ctx.artist, "Test Artist"));
    assert(!strcmp(ctx.album, "Test Disc"));
    append_status(&ctx, status, sizeof(status));
    assert(strstr(status, "\"playback_state\":\"playing\""));
    assert(strstr(status, "\"source\":\"airplay\""));
    assert(strstr(status, "\"title\":\"LibreEcho \\\"Live\\\"\\u000a\""));
    assert(strstr(status, "\"artist\":\"Test Artist\""));
    assert(strstr(status, "\"album\":\"Test Disc\""));
}

static void test_missing_metadata_and_session_clear(void)
{
    static const char begin[] =
        "<item><type>ssnc</type><code>pbeg</code>"
        "<length>0</length></item>";
    static const char end[] =
        "<item><type>ssnc</type><code>pend</code>"
        "<length>0</length></item>";
    struct airplay_ctx ctx;
    char status[512];

    init_ctx(&ctx);
    strcpy(ctx.title, "stale title");
    metadata_parser_feed(&ctx, begin, strlen(begin));
    assert(ctx.playing == 1);
    assert(ctx.title[0] == '\0');
    append_status(&ctx, status, sizeof(status));
    assert(strstr(status, "\"playback_state\":\"playing\""));
    assert(strstr(status, "\"source\":\"airplay\""));
    assert(!strstr(status, "\"title\""));
    strcpy(ctx.artist, "stale artist");
    metadata_parser_feed(&ctx, end, strlen(end));
    assert(ctx.playing == 0);
    assert(ctx.artist[0] == '\0');
    append_status(&ctx, status, sizeof(status));
    assert(strstr(status, "\"playback_state\":\"stopped\""));
    assert(!strstr(status, "\"source\""));
    assert(!strstr(status, "\"artist\""));
}

static void test_ap2_now_playing_plist(void)
{
    static const char begin[] =
        "<item><type>ssnc</type><code>pres</code>"
        "<length>0</length></item>";
    static const char command[] =
        "<item><type>ssnc</type><code>copl</code>"
        "<length>288</length><data encoding=\"base64\">"
        "YnBsaXN0MDDSAQIDDlZwYXJhbXNUdHlwZdMEAQIFBg1bbWVyZ2VQb2xpY3lX"
        "cmVwbGFjZdMHCAkKCwxfECFrTVJNZWRpYVJlbW90ZU5vd1BsYXlpbmdJbmZv"
        "QWxidW1fECJrTVJNZWRpYVJlbW90ZU5vd1BsYXlpbmdJbmZvQXJ0aXN0XxAh"
        "a01STWVkaWFSZW1vdGVOb3dQbGF5aW5nSW5mb1RpdGxlXxAUSHVudGluZyBI"
        "aWdoIGFuZCBMb3dUYS1oYVpUYWtlIG9uIE1lWG5waS10ZXh0XxAWdXBkYXRl"
        "TVJOb3dQbGF5aW5nSW5mbwgNFBkgLDQ7X4Sov8TP2AAAAAAAAAEBAAAAAAAA"
        "AA8AAAAAAAAAAAAAAAAAAADx"
        "</data></item>";
    struct airplay_ctx ctx;
    char status[2048];

    init_ctx(&ctx);
    feed_fragmented(&ctx, begin, 4);
    feed_fragmented(&ctx, command, 11);
    assert(ctx.playing == 1);
    assert(!strcmp(ctx.title, "Take on Me"));
    assert(!strcmp(ctx.artist, "a-ha"));
    assert(!strcmp(ctx.album, "Hunting High and Low"));
    append_status(&ctx, status, sizeof(status));
    assert(strstr(status, "\"title\":\"Take on Me\""));
    assert(strstr(status, "\"artist\":\"a-ha\""));
    assert(strstr(status, "\"album\":\"Hunting High and Low\""));
}

static void test_malformed_input_recovers(void)
{
    static const char malformed[] =
        "garbage<item><type>core</type><code>minm</code>"
        "<length>5</length><data encoding=\"base64\">"
        "!!!!</data></item>";
    static const char valid[] =
        "<item><type>core</type><code>minm</code>"
        "<length>5</length><data encoding=\"base64\">"
        "QWZ0ZXI=</data></item>";
    static const char wrong_length[] =
        "<item><type>core</type><code>asar</code>"
        "<length>6</length><data encoding=\"base64\">"
        "U2hvcnQ=</data></item>";
    struct airplay_ctx ctx;

    init_ctx(&ctx);
    metadata_parser_feed(&ctx, malformed, strlen(malformed));
    assert(ctx.title[0] == '\0');
    metadata_parser_feed(&ctx, wrong_length, strlen(wrong_length));
    assert(ctx.artist[0] == '\0');
    feed_fragmented(&ctx, valid, 4);
    assert(!strcmp(ctx.title, "After"));
}

static void test_oversized_item_recovers(void)
{
    static const char valid[] =
        "<item><type>core</type><code>asal</code>"
        "<length>2</length><data encoding=\"base64\">"
        "T0s=</data></item>";
    struct airplay_ctx ctx;
    char chunk[257];
    size_t remaining = AIRPLAY_METADATA_ITEM_MAX + 128;

    init_ctx(&ctx);
    metadata_parser_feed(&ctx, "<item>", 6);
    memset(chunk, 'x', sizeof(chunk));
    while (remaining > 0) {
        size_t count = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        metadata_parser_feed(&ctx, chunk, count);
        remaining -= count;
    }
    metadata_parser_feed(&ctx, "</item>", 7);
    assert(ctx.metadata_parser.used == 0);
    feed_fragmented(&ctx, valid, 7);
    assert(!strcmp(ctx.album, "OK"));
}

static void test_oversized_field_is_ignored(void)
{
    static const char oversized[] =
        "<item><type>core</type><code>minm</code>"
        "<length>193</length><data encoding=\"base64\">"
        "QQ==</data></item>";
    static const char invalid_utf8[] =
        "<item><type>core</type><code>asar</code>"
        "<length>2</length><data encoding=\"base64\">"
        "wyg=</data></item>";
    struct airplay_ctx ctx;

    init_ctx(&ctx);
    metadata_parser_feed(&ctx, oversized, strlen(oversized));
    metadata_parser_feed(&ctx, invalid_utf8, strlen(invalid_utf8));
    assert(ctx.title[0] == '\0');
    assert(ctx.artist[0] == '\0');
}

static void test_fifo_is_nonblocking(void)
{
    static const char begin[] =
        "<item><type>ssnc</type><code>pbeg</code>"
        "<length>0</length></item>";
    struct airplay_ctx ctx;
    struct stat status;
    char directory[] = "/tmp/libreecho-airplay-metadata.XXXXXX";
    int flags;

    init_ctx(&ctx);
    assert(mkdtemp(directory) != NULL);
    assert(snprintf(ctx.metadata_path, sizeof(ctx.metadata_path),
                    "%s/metadata", directory) > 0);
    assert(metadata_fifo_open(&ctx) == 0);
    assert(lstat(ctx.metadata_path, &status) == 0);
    assert(S_ISFIFO(status.st_mode));
    flags = fcntl(ctx.metadata_fd, F_GETFL);
    assert(flags >= 0);
    assert((flags & O_NONBLOCK) != 0);
    assert(write(ctx.metadata_fd, begin, sizeof(begin) - 1) ==
           (ssize_t)(sizeof(begin) - 1));
    metadata_fifo_drain(&ctx);
    assert(ctx.playing == 1);
    metadata_fifo_close(&ctx);
    assert(unlink(ctx.metadata_path) == 0);
    assert(rmdir(directory) == 0);
}

static void test_hostname_refresh_is_noop_while_disabled(void)
{
    struct airplay_ctx ctx;
    char message[] = "{\"v\":1,\"id\":17,\"cmd\":\"refresh_hostname\",\"args\":{}}";
    char response[512];
    int length;

    init_ctx(&ctx);
    ctx.enabled = 0;
    length = request(&ctx, message, response, sizeof(response));
    assert(length > 0);
    assert(strstr(response, "\"id\":17"));
    assert(strstr(response, "\"ok\":true"));
    assert(ctx.enabled == 0);
}

static void test_hostname_refresh_failure_remains_retryable(void)
{
    struct airplay_ctx ctx;
    char first_message[] = "{\"v\":1,\"id\":18,\"cmd\":\"refresh_hostname\",\"args\":{}}";
    char retry_message[] = "{\"v\":1,\"id\":19,\"cmd\":\"refresh_hostname\",\"args\":{}}";
    char response[512];
    int length;

    init_ctx(&ctx);
    ctx.enabled = 1;
    ctx.engine_pid = -1;
    length = request(&ctx, first_message, response, sizeof(response));
    assert(length > 0);
    assert(strstr(response, "\"id\":18"));
    assert(strstr(response, "\"ok\":false"));
    assert(strstr(response, "hostname refresh failed"));
    assert(ctx.enabled == 1);

    length = request(&ctx, retry_message, response, sizeof(response));
    assert(length > 0);
    assert(strstr(response, "\"id\":19"));
    assert(strstr(response, "\"ok\":false"));
    assert(strstr(response, "hostname refresh failed"));
    assert(ctx.enabled == 1);
}

/* The controller no longer owns the discovery stack, so a hostname refresh is
 * an external-dependency check: it must not restart the AirPlay children and
 * it must report the shared supervisor's readiness. */
static void test_hostname_refresh_follows_shared_supervisor(void)
{
    struct airplay_ctx ctx;
    char message[] = "{\"v\":1,\"id\":22,\"cmd\":\"refresh_hostname\",\"args\":{}}";
    char response[512];
    pid_t supervisor;
    int length;

    init_ctx(&ctx);
    ctx.enabled = 1;
    ctx.shairport_pid = spawn_status_supervisor(ctx.mdns_socket, "running\n");
    supervisor = ctx.shairport_pid;
    assert(mdns_ready(&ctx) == 1);
    length = request(&ctx, message, response, sizeof(response));
    assert(length > 0);
    assert(strstr(response, "\"id\":22"));
    assert(strstr(response, "\"ok\":true"));
    /* Discovery is external: the AirPlay children keep running. */
    assert(kill(ctx.shairport_pid, 0) == 0);
    assert(ctx.enabled == 1);
    kill(supervisor, SIGKILL);
    waitpid(supervisor, NULL, 0);
    ctx.shairport_pid = -1;
}

static void test_disabling_airplay_leaves_shared_mdns_alone(void)
{
    struct airplay_ctx ctx;
    char message[] = "{\"v\":1,\"id\":20,\"cmd\":\"status\",\"args\":{}}";
    char response[1024];
    int length;

    init_ctx(&ctx);
    ctx.enabled = 1;
    assert(set_enabled(&ctx, 0) == 0);
    assert(ctx.enabled == 0);
    length = request(&ctx, message, response, sizeof(response));
    assert(length > 0);
    assert(strstr(response, "\"enabled\":false"));
    /* An absent shared supervisor is reported, never spawned or stopped here. */
    assert(strstr(response, "\"mdns_running\":false"));
}

static void test_ready_shared_supervisor_is_reported(void)
{
    struct airplay_ctx ctx;
    char message[] = "{\"v\":1,\"id\":23,\"cmd\":\"status\",\"args\":{}}";
    char response[1024];
    pid_t supervisor;
    int length;

    init_ctx(&ctx);
    supervisor = spawn_status_supervisor(ctx.mdns_socket, "running\n");
    assert(mdns_ready(&ctx) == 1);
    length = request(&ctx, message, response, sizeof(response));
    assert(length > 0);
    assert(strstr(response, "\"mdns_running\":true"));
    kill(supervisor, SIGKILL);
    waitpid(supervisor, NULL, 0);
}

static void test_degraded_shared_supervisor_is_not_ready(void)
{
    struct airplay_ctx ctx;
    pid_t supervisor;

    init_ctx(&ctx);
    supervisor = spawn_status_supervisor(ctx.mdns_socket, "degraded\n");
    assert(mdns_ready(&ctx) == 0);
    kill(supervisor, SIGKILL);
    waitpid(supervisor, NULL, 0);
    /* No supervisor at all is equally not ready, and is never fatal. */
    assert(mdns_ready(&ctx) == 0);
}

int main(void)
{
    test_sender_master_session_hooks();
    test_sender_master_mapping();
    test_sender_master_poll();
    test_master_status_timeout_preserves_button_change();
    test_master_write_timeout_still_ends();
    test_master_rejected_write_stays_gated();
    test_two_sessions_real_audiod_transport();
    test_fragmented_base64_and_json();
    test_missing_metadata_and_session_clear();
    test_ap2_now_playing_plist();
    test_malformed_input_recovers();
    test_oversized_item_recovers();
    test_oversized_field_is_ignored();
    test_fifo_is_nonblocking();
    test_hostname_refresh_is_noop_while_disabled();
    test_hostname_refresh_failure_remains_retryable();
    test_hostname_refresh_follows_shared_supervisor();
    test_disabling_airplay_leaves_shared_mdns_alone();
    test_ready_shared_supervisor_is_reported();
    test_degraded_shared_supervisor_is_not_ready();
    puts("airplay discovery is an external shared mDNS dependency: ok");
    return 0;
}
