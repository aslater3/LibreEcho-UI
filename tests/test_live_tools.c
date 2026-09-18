#define _POSIX_C_SOURCE 200809L

/*
 * Delegation allow-list and argument validation.
 *
 * The security property under test is that a model asking for something is not
 * the same as the device doing it: unknown tools, malformed arguments and
 * anything carrying a path or a shell metacharacter must be refused before a
 * sibling daemon is contacted.
 *
 * The successful-dispatch cases run the calling side in this process and the
 * answering side in a forked child, which is what the real topology looks like
 * and lets the assertions inspect the exact adapter request a daemon received.
 */

#include "adapter/live_tools.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static char directory[64];
static int refusals_reached_no_daemon;

static int join_path(char *out, size_t size, const char *dir, const char *name)
{
    size_t dir_length = strlen(dir);
    size_t name_length = strlen(name);

    if (dir_length + 1 + name_length + 1 > size)
        return -1;
    memcpy(out, dir, dir_length);
    out[dir_length] = '/';
    memcpy(out + dir_length + 1, name, name_length);
    out[dir_length + 1 + name_length] = '\0';
    return 0;
}

static int write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *position = buffer;

    while (size) {
        ssize_t count = write(fd, position, size);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        position += count;
        size -= (size_t)count;
    }
    return 0;
}

struct fake_daemon {
    int fd;
    char path[128];
    char response[256];
    pid_t child;
    int request_fd;
};

static int fake_listen(struct fake_daemon *daemon, const char *name,
                       const char *response)
{
    struct sockaddr_un address;

    memset(daemon, 0, sizeof(*daemon));
    daemon->fd = -1;
    daemon->request_fd = -1;
    if (join_path(daemon->path, sizeof(daemon->path), directory, name) < 0)
        return -1;
    snprintf(daemon->response, sizeof(daemon->response), "%s", response);
    daemon->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (daemon->fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(daemon->path) >= sizeof(address.sun_path))
        return -1;
    memcpy(address.sun_path, daemon->path, strlen(daemon->path) + 1);
    unlink(daemon->path);
    if (bind(daemon->fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(daemon->fd, 4) < 0)
        return -1;
    return 0;
}

/* Pull the numeric request id out of an adapter request line. */
static unsigned long request_id(const char *request)
{
    const char *position = strstr(request, "\"id\":");

    if (!position)
        return 0;
    return strtoul(position + 5, NULL, 10);
}

/*
 * Fork a worker that answers exactly `requests` adapter requests, echoing each
 * request it received back up a pipe so the parent can assert on it.
 */
static int fake_spawn(struct fake_daemon *daemon, unsigned int requests)
{
    int fds[2];
    pid_t pid;

    if (pipe(fds) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        unsigned int i;

        close(fds[0]);
        /*
         * Never block forever.  If the caller makes fewer requests than the
         * worker was told to expect - because an early check failed, or a
         * refusal stopped the dispatch before it reached this daemon - a
         * blocking accept() would leave this process parked on the socket for
         * the rest of the machine's uptime.  A deadline turns that into a
         * worker that exits, which the parent's bounded read then reports.
         */
        alarm(20);
        for (i = 0; i < requests; ++i) {
            char request[1024];
            char response[512];
            size_t used = 0;
            int client = accept(daemon->fd, NULL, NULL);

            if (client < 0)
                break;
            while (used + 1 < sizeof(request)) {
                ssize_t count = read(client, request + used, 1);

                if (count != 1)
                    break;
                if (request[used++] == '\n')
                    break;
            }
            request[used] = '\0';
            (void)write_all(fds[1], request, strlen(request));
            (void)write_all(fds[1], "\n", 1);
            snprintf(response, sizeof(response), daemon->response,
                     request_id(request));
            strncat(response, "\n", sizeof(response) - strlen(response) - 1);
            (void)write_all(client, response, strlen(response));
            close(client);
        }
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    daemon->child = pid;
    daemon->request_fd = fds[0];
    return 0;
}

/* Wait for the worker and collect the requests it saw, newline separated. */
static int fake_collect(struct fake_daemon *daemon, char *out, size_t size)
{
    size_t used = 0;
    int status = 0;
    int i;

    if (daemon->request_fd < 0 || !out || !size)
        return -1;
    out[0] = '\0';
    /*
     * Bounded read.  A pipe whose writer is still alive blocks forever, and a
     * test that hangs tells you less than one that fails.
     */
    for (i = 0; i < 200 && used + 1 < size; ++i) {
        struct pollfd descriptor;
        ssize_t count;

        descriptor.fd = daemon->request_fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        if (poll(&descriptor, 1, 50) <= 0)
            continue;
        count = read(daemon->request_fd, out + used, size - 1 - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        used += (size_t)count;
    }
    out[used] = '\0';
    if (!used) {
        /*
         * Nothing arrived: the worker was left waiting for a request that was
         * never sent.  Report it here rather than letting the caller assert on
         * an empty buffer and describe the wrong failure.
         */
        fprintf(stderr, "fake daemon received no request\n");
        return -1;
    }
    close(daemon->request_fd);
    daemon->request_fd = -1;
    if (daemon->child > 0)
        (void)waitpid(daemon->child, &status, 0);
    daemon->child = -1;
    return 0;
}

static void fake_close(struct fake_daemon *daemon)
{
    if (daemon->fd >= 0)
        close(daemon->fd);
    daemon->fd = -1;
    unlink(daemon->path);
}

static int test_allow_list(void)
{
    unsigned int index = 0;
    const char *name;

    CHECK(le_live_tools_supported("timer.set"));
    CHECK(le_live_tools_supported("media.stop"));
    CHECK(le_live_tools_supported("device.volume"));
    /* Anything a model might reach for that is not published. */
    CHECK(!le_live_tools_supported("shell.exec"));
    CHECK(!le_live_tools_supported("fs.read"));
    CHECK(!le_live_tools_supported("codex.exec"));
    CHECK(!le_live_tools_supported(""));
    CHECK(!le_live_tools_supported(NULL));
    CHECK(!le_live_tools_supported("timer.set;rm -rf /"));
    CHECK(!le_live_tools_supported("timer.set/../../etc/passwd"));
    /* The published list must be the same table the dispatcher enforces. */
    while ((name = le_live_tools_name(index)) != NULL) {
        CHECK(le_live_tools_supported(name));
        ++index;
    }
    CHECK(index >= 8);
    return 0;
}

static int test_refusals_do_not_touch_daemons(void)
{
    struct le_live_tool_environment environment;
    char result[LE_LIVE_TOOL_RESULT_MAX];

    le_live_tools_init(&environment);
    /* Point every socket at a path that cannot exist. */
    CHECK(join_path(environment.timer_socket, sizeof(environment.timer_socket),
                    directory, "absent-timer.sock") == 0);
    CHECK(join_path(environment.radio_socket, sizeof(environment.radio_socket),
                    directory, "absent-radio.sock") == 0);
    CHECK(join_path(environment.audio_socket, sizeof(environment.audio_socket),
                    directory, "absent-audio.sock") == 0);

    /* Unknown tool: refused, and it says so speakably. */
    CHECK(le_live_tools_dispatch(&environment, "shell.exec", "{}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "\"ok\":false") != NULL);
    CHECK(strstr(result, "not available") != NULL);

    /* Malformed arguments: refused before any handler runs. */
    CHECK(le_live_tools_dispatch(&environment, "timer.set", "{\"seconds\":",
                                 result, sizeof(result)) < 0);
    CHECK(le_live_tools_dispatch(&environment, "timer.set", "not json", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "malformed") != NULL);

    /* A duration a sane timer cannot honour is refused, not clamped. */
    CHECK(le_live_tools_dispatch(&environment, "timer.set", "{\"seconds\":0}",
                                 result, sizeof(result)) < 0);
    CHECK(le_live_tools_dispatch(&environment, "timer.set",
                                 "{\"seconds\":999999}", result,
                                 sizeof(result)) < 0);
    CHECK(le_live_tools_dispatch(&environment, "timer.set", "{}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "timer") != NULL);

    /* A label carrying a path must not reach a daemon. */
    CHECK(le_live_tools_dispatch(
              &environment, "timer.set",
              "{\"seconds\":60,\"label\":\"/etc/passwd\"}", result,
              sizeof(result)) < 0);
    CHECK(strstr(result, "label") != NULL);

    /* Only internet radio addresses may be played. */
    CHECK(le_live_tools_dispatch(&environment, "radio.play",
                                 "{\"url\":\"file:///etc/passwd\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "internet radio") != NULL);
    CHECK(le_live_tools_dispatch(&environment, "radio.play", "{}", result,
                                 sizeof(result)) < 0);

    /* A read-only tool with no source is a bounded failure, not a crash. */
    CHECK(le_live_tools_dispatch(&environment, "device.weather", "{}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "\"ok\":false") != NULL);

    /* Every refusal produced a bounded result object. */
    CHECK(strlen(result) < sizeof(result));
    return 0;
}

static int test_timer_dispatch_reaches_the_daemon_with_validated_fields(void)
{
    struct le_live_tool_environment environment;
    struct fake_daemon timer;
    char result[LE_LIVE_TOOL_RESULT_MAX];
    char requests[2048];

    CHECK(fake_listen(&timer, "timer.sock",
                      "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{\"id\":3}}") == 0);
    CHECK(fake_spawn(&timer, 5) == 0);
    le_live_tools_init(&environment);
    snprintf(environment.timer_socket, sizeof(environment.timer_socket), "%s",
             timer.path);
    environment.timeout_ms = 1000;

    CHECK(le_live_tools_dispatch(&environment, "timer.set",
                                 "{\"seconds\":600,\"label\":\"pasta\"}",
                                 result, sizeof(result)) == 0);
    CHECK(strstr(result, "\"ok\":true") != NULL);
    CHECK(le_live_tools_dispatch(&environment, "timer.set",
                                 "{\"seconds\":60,\"label\":\"a\\\"b\"}",
                                 result, sizeof(result)) == 0);
    CHECK(strstr(result, "a\\\"b") != NULL);
    CHECK(le_live_tools_dispatch(&environment, "timer.cancel", "{\"id\":3}",
                                 result, sizeof(result)) == 0);
    CHECK(le_live_tools_dispatch(&environment, "timer.cancel", "{}", result,
                                 sizeof(result)) == 0);
    CHECK(le_live_tools_dispatch(&environment, "timer.dismiss", "{}", result,
                                 sizeof(result)) == 0);

    CHECK(fake_collect(&timer, requests, sizeof(requests)) == 0);
    /* The daemon saw the validated fields, and the right command each time. */
    CHECK(strstr(requests, "\"cmd\":\"add\"") != NULL);
    CHECK(strstr(requests, "\"seconds\":600") != NULL);
    CHECK(strstr(requests, "pasta") != NULL);
    CHECK(strstr(requests, "a\\\"b") != NULL);
    CHECK(strstr(requests, "\"cmd\":\"cancel\"") != NULL);
    CHECK(strstr(requests, "\"cmd\":\"cancel_all\"") != NULL);
    CHECK(strstr(requests, "\"cmd\":\"dismiss\"") != NULL);
    fake_close(&timer);
    return 0;
}


static int test_voice_request_routes_to_existing_timer_tool(void)
{
    struct le_live_tool_environment environment;
    struct fake_daemon timer;
    char result[LE_LIVE_TOOL_RESULT_MAX];
    char requests[1024];

    CHECK(fake_listen(&timer, "voice-timer.sock",
                      "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{\"id\":9}}") == 0);
    CHECK(fake_spawn(&timer, 1) == 0);
    le_live_tools_init(&environment);
    snprintf(environment.timer_socket, sizeof(environment.timer_socket), "%s",
             timer.path);
    environment.timeout_ms = 1000;

    CHECK(le_live_tools_dispatch(&environment, "voice.request",
                                 "{\"request\":\"set a timer for ten minutes\"}",
                                 result, sizeof(result)) == 0);
    CHECK(strstr(result, "\"ok\":true") != NULL);
    CHECK(fake_collect(&timer, requests, sizeof(requests)) == 0);
    CHECK(strstr(requests, "\"cmd\":\"add\"") != NULL);
    CHECK(strstr(requests, "\"seconds\":600") != NULL);
    fake_close(&timer);
    return 0;
}

static int test_timer_query_reports_bounded_state(void)
{
    struct le_live_tool_environment environment;
    struct fake_daemon timer;
    char result[LE_LIVE_TOOL_RESULT_MAX];
    char requests[1024];

    CHECK(fake_listen(&timer, "timer-query.sock",
                      "{\"v\":1,\"id\":1,\"ok\":true,\"data\":"
                      "{\"pending\":2,\"next_seconds\":45}}") == 0);
    CHECK(fake_spawn(&timer, 1) == 0);
    le_live_tools_init(&environment);
    snprintf(environment.timer_socket, sizeof(environment.timer_socket), "%s",
             timer.path);
    environment.timeout_ms = 1000;

    CHECK(le_live_tools_dispatch(&environment, "timer.query", "{}", result,
                                 sizeof(result)) == 0);
    CHECK(strstr(result, "\"pending\":2") != NULL);
    CHECK(strstr(result, "\"next_seconds\":45") != NULL);
    CHECK(fake_collect(&timer, requests, sizeof(requests)) == 0);
    CHECK(strstr(requests, "\"cmd\":\"status\"") != NULL);
    fake_close(&timer);
    return 0;
}

static int test_device_time_uses_the_clock(void)
{
    struct le_live_tool_environment environment;
    char result[LE_LIVE_TOOL_RESULT_MAX];
    char status_path[160];
    FILE *status;

    le_live_tools_init(&environment);
    CHECK(join_path(status_path, sizeof(status_path), directory,
                    "time.status") == 0);
    snprintf(environment.time_status_path,
             sizeof(environment.time_status_path), "%s", status_path);
    status = fopen(status_path, "w");
    CHECK(status != NULL);
    fputs("state=synchronized\nsynchronized=1\n", status);
    fclose(status);

    CHECK(le_live_tools_dispatch(&environment, "device.time", "{}", result,
                                 sizeof(result)) == 0);
    CHECK(strstr(result, "\"time\":\"") != NULL);
    CHECK(strstr(result, "\"synchronized\":true") != NULL);
    unlink(status_path);
    return 0;
}

static int test_device_volume_reads_the_audio_daemon(void)
{
    struct le_live_tool_environment environment;
    struct fake_daemon audio;
    char result[LE_LIVE_TOOL_RESULT_MAX];
    char requests[1024];

    CHECK(fake_listen(&audio, "volume.sock",
                      "{\"v\":1,\"id\":1,\"ok\":true,\"data\":"
                      "{\"volume\":37,\"muted\":false}}") == 0);
    CHECK(fake_spawn(&audio, 1) == 0);
    le_live_tools_init(&environment);
    snprintf(environment.audio_socket, sizeof(environment.audio_socket), "%s",
             audio.path);
    environment.timeout_ms = 1000;

    CHECK(le_live_tools_dispatch(&environment, "device.volume", "{}", result,
                                 sizeof(result)) == 0);
    CHECK(strstr(result, "\"volume\":37") != NULL);
    CHECK(strstr(result, "\"muted\":false") != NULL);
    CHECK(fake_collect(&audio, requests, sizeof(requests)) == 0);
    CHECK(strstr(requests, "\"cmd\":\"status\"") != NULL);
    fake_close(&audio);
    return 0;
}

static int test_media_stop_reports_partial_failure(void)
{
    struct le_live_tool_environment environment;
    struct fake_daemon audio;
    char result[LE_LIVE_TOOL_RESULT_MAX];
    char requests[1024];

    CHECK(fake_listen(&audio, "audio.sock",
                      "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{}}") == 0);
    CHECK(fake_spawn(&audio, 2) == 0);
    le_live_tools_init(&environment);
    snprintf(environment.audio_socket, sizeof(environment.audio_socket), "%s",
             audio.path);
    /* Radio is unreachable: stopping is still reported as success because at
       least one playback owner answered. */
    CHECK(join_path(environment.radio_socket, sizeof(environment.radio_socket),
                    directory, "absent-radio.sock") == 0);
    environment.timeout_ms = 300;

    CHECK(le_live_tools_dispatch(&environment, "media.stop", "{}", result,
                                 sizeof(result)) == 0);
    CHECK(strstr(result, "\"ok\":true") != NULL);
    CHECK(fake_collect(&audio, requests, sizeof(requests)) == 0);
    CHECK(strstr(requests, "\"cmd\":\"stop_speech\"") != NULL);
    CHECK(strstr(requests, "\"cmd\":\"noise_stop\"") != NULL);
    fake_close(&audio);

    /* With nothing reachable at all the answer must be a failure. */
    CHECK(join_path(environment.audio_socket, sizeof(environment.audio_socket),
                    directory, "absent-audio.sock") == 0);
    CHECK(le_live_tools_dispatch(&environment, "media.stop", "{}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "\"ok\":false") != NULL);
    return 0;
}

static int test_spoken_stop_requests_session_end(void)
{
    struct le_live_tool_environment environment;
    struct fake_daemon audio;
    char result[LE_LIVE_TOOL_RESULT_MAX];
    char requests[1024];

    CHECK(fake_listen(&audio, "voice-stop-audio.sock",
                      "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{}}") == 0);
    CHECK(fake_spawn(&audio, 2) == 0);
    le_live_tools_init(&environment);
    snprintf(environment.audio_socket, sizeof(environment.audio_socket), "%s",
             audio.path);
    CHECK(join_path(environment.radio_socket, sizeof(environment.radio_socket),
                    directory, "absent-voice-stop-radio.sock") == 0);
    environment.timeout_ms = 300;

    CHECK(le_live_tools_dispatch(&environment, "voice.request",
                                 "{\"request\":\"stop\"}", result,
                                 sizeof(result)) == 0);
    CHECK(strstr(result, "\"end_session\":true") != NULL);
    CHECK(fake_collect(&audio, requests, sizeof(requests)) == 0);
    CHECK(strstr(requests, "\"cmd\":\"stop_speech\"") != NULL);
    fake_close(&audio);
    return 0;
}

static int test_session_stop_returns_end_session(void)
{
    struct le_live_tool_environment environment;
    char result[LE_LIVE_TOOL_RESULT_MAX];

    le_live_tools_init(&environment);
    CHECK(le_live_tools_dispatch(&environment, "session.stop", "{}", result,
                                 sizeof(result)) == 0);
    CHECK(strstr(result, "\"end_session\":true") != NULL);
    return 0;
}

static int test_bounded_result_for_a_long_argument(void)
{
    struct le_live_tool_environment environment;
    char result[LE_LIVE_TOOL_RESULT_MAX];
    char arguments[LE_LIVE_ARGUMENT_MAX];
    size_t i;

    le_live_tools_init(&environment);
    CHECK(join_path(environment.radio_socket, sizeof(environment.radio_socket),
                    directory, "absent-radio.sock") == 0);
    /* A url longer than the field must be refused, not truncated into use. */
    memcpy(arguments, "{\"url\":\"http://", 14);
    for (i = 14; i < sizeof(arguments) - 4; ++i)
        arguments[i] = 'a';
    memcpy(arguments + sizeof(arguments) - 3, "\"}", 3);
    CHECK(le_live_tools_dispatch(&environment, "radio.play", arguments, result,
                                 sizeof(result)) < 0);
    CHECK(strlen(result) < sizeof(result));
    return 0;
}

int main(void)
{
    int failures = 0;

    snprintf(directory, sizeof(directory), "/tmp/le-live-tools-%d",
             (int)getpid());
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create %s\n", directory);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    failures += test_allow_list() != 0;
    failures += test_refusals_do_not_touch_daemons() != 0;
    failures += test_timer_dispatch_reaches_the_daemon_with_validated_fields() != 0;
    failures += test_voice_request_routes_to_existing_timer_tool() != 0;
    failures += test_timer_query_reports_bounded_state() != 0;
    failures += test_device_time_uses_the_clock() != 0;
    failures += test_device_volume_reads_the_audio_daemon() != 0;
    failures += test_media_stop_reports_partial_failure() != 0;
    failures += test_spoken_stop_requests_session_end() != 0;
    failures += test_session_stop_returns_end_session() != 0;
    failures += test_bounded_result_for_a_long_argument() != 0;
    (void)refusals_reached_no_daemon;
    rmdir(directory);
    if (failures) {
        fprintf(stderr, "live tools: FAILED\n");
        return 1;
    }
    printf("live tools: ok\n");
    return 0;
}
