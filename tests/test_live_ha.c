#define _POSIX_C_SOURCE 200809L

/*
 * Home Assistant delegation for GPT-Live: the REST client and the ha.* tools.
 *
 * A fake Home Assistant runs in a forked child on 127.0.0.1 with a scripted
 * sequence of responses.  The child writes every request it received back up
 * a pipe, so each test asserts the exact method, path, bearer token and body
 * the real client sent, and that refused requests never reached the server.
 * No external host is contacted.
 */

#include "adapter/live_ha.h"
#include "adapter/live_tools.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define TOKEN "eyJhbGciOi.test_token-0123456789"
#define RECORD_END '\x1e'
#define SERVER_REQUEST_MAX 2048

struct ha_script {
    int status;
    const char *body;
    int chunked;
};

struct ha_server {
    int pipe_fd;
    pid_t pid;
    unsigned short port;
};

static char directory[96];
static char token_path[160];
static char missing_path[160];

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

/* Read one HTTP request (headers plus Content-Length body) from a client. */
static long read_request(int client, char *out, size_t size)
{
    size_t used = 0;
    size_t body_wanted = 0;
    const char *header_end = NULL;

    while (used + 1 < size) {
        ssize_t count = read(client, out + used, 1);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        ++used;
        out[used] = '\0';
        if (!header_end && used >= 4 && !strcmp(out + used - 4, "\r\n\r\n")) {
            const char *length = strstr(out, "Content-Length: ");

            header_end = out + used;
            body_wanted = length ? strtoul(length + 16, NULL, 10) : 0U;
            if (!body_wanted)
                break;
        }
        if (header_end && used >= (size_t)(header_end - out) + body_wanted)
            break;
    }
    return (long)used;
}

/*
 * Fork the fake Home Assistant.  It answers exactly `count` requests in order,
 * then exits.  The alarm bounds it if the client sends fewer than expected.
 */
static int ha_server_start(struct ha_server *server, const struct ha_script *script,
                           unsigned int count)
{
    struct sockaddr_in address;
    socklen_t address_size = sizeof(address);
    int fds[2];
    int listener;
    pid_t pid;

    memset(server, 0, sizeof(*server));
    server->pipe_fd = -1;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 4) < 0 ||
        getsockname(listener, (struct sockaddr *)&address, &address_size) < 0 ||
        pipe(fds) < 0) {
        close(listener);
        return -1;
    }
    server->port = ntohs(address.sin_port);
    pid = fork();
    if (pid < 0) {
        close(listener);
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        unsigned int i;

        close(fds[0]);
        alarm(10);
        for (i = 0; i < count; ++i) {
            char request[SERVER_REQUEST_MAX];
            char response[1024];
            long used;
            int client = accept(listener, NULL, NULL);

            if (client < 0)
                break;
            used = read_request(client, request, sizeof(request));
            if (used > 0) {
                (void)write_all(fds[1], request, (size_t)used);
                (void)write_all(fds[1], &(char){RECORD_END}, 1);
            }
            if (script[i].chunked) {
                size_t body = strlen(script[i].body);
                size_t half = body / 2;
                int written = snprintf(response, sizeof(response),
                    "HTTP/1.1 %d X\r\nTransfer-Encoding: chunked\r\n"
                    "Connection: close\r\n\r\n%zx\r\n%.*s\r\n%zx\r\n%s\r\n0\r\n\r\n",
                    script[i].status, half, (int)half, script[i].body,
                    body - half, script[i].body + half);
                if (written > 0)
                    (void)write_all(client, response, (size_t)written);
            } else {
                int written = snprintf(response, sizeof(response),
                    "HTTP/1.1 %d X\r\nContent-Length: %zu\r\n"
                    "Connection: close\r\n\r\n%s",
                    script[i].status, strlen(script[i].body), script[i].body);
                if (written > 0)
                    (void)write_all(client, response, (size_t)written);
            }
            close(client);
        }
        close(listener);
        close(fds[1]);
        _exit(0);
    }
    close(listener);
    close(fds[1]);
    server->pid = pid;
    server->pipe_fd = fds[0];
    return 0;
}

/* Collect what the server saw, RECORD_END separated, and reap it. */
static int ha_server_finish(struct ha_server *server, char *out, size_t size)
{
    size_t used = 0;
    int status = 0;
    int i;

    if (!out || !size)
        return -1;
    out[0] = '\0';
    for (i = 0; i < 200 && used + 1 < size; ++i) {
        struct pollfd descriptor;
        ssize_t count;

        descriptor.fd = server->pipe_fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        if (poll(&descriptor, 1, 100) < 0)
            break;
        if (!(descriptor.revents & (POLLIN | POLLHUP)))
            continue;
        count = read(server->pipe_fd, out + used, size - 1 - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        used += (size_t)count;
        out[used] = '\0';
    }
    close(server->pipe_fd);
    server->pipe_fd = -1;
    waitpid(server->pid, &status, 0);
    return 0;
}

static int write_file(const char *path, const char *content, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    size_t length = strlen(content);

    if (fd < 0)
        return -1;
    if (write_all(fd, content, length) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return chmod(path, mode);
}

static int setup_directory(void)
{
    snprintf(directory, sizeof(directory), "/tmp/le-live-ha-%ld", (long)getpid());
    mkdir(directory, 0700);
    snprintf(token_path, sizeof(token_path), "%s/ha-token", directory);
    snprintf(missing_path, sizeof(missing_path), "%s/absent-token", directory);
    return write_file(token_path, TOKEN "\n", 0600) == 0 ? 0 : -1;
}

static void cleanup_directory(void)
{
    char path[200];

    snprintf(path, sizeof(path), "%s/ha-token", directory);
    unlink(path);
    rmdir(directory);
}

static void environment_for(struct le_live_tool_environment *environment,
                            unsigned short port, const char *token)
{
    le_live_tools_init(environment);
    if (port)
        snprintf(environment->ha_url, sizeof(environment->ha_url),
                 "http://127.0.0.1:%u", (unsigned int)port);
    snprintf(environment->ha_token_path, sizeof(environment->ha_token_path),
             "%s", token);
}

/* --- token loading and URL handling -------------------------------------- */

static int test_token_loading(void)
{
    char token[LE_LIVE_HA_TOKEN_MAX];
    char path[200];

    CHECK(le_live_ha_load_token(token_path, token, sizeof(token)) == 0);
    CHECK(!strcmp(token, TOKEN));
    CHECK(le_live_ha_load_token(missing_path, token, sizeof(token)) < 0);

    snprintf(path, sizeof(path), "%s/short-token", directory);
    CHECK(write_file(path, "short\n", 0600) == 0);
    CHECK(le_live_ha_load_token(path, token, sizeof(token)) < 0);
    CHECK(write_file(path, "has space in it 0123456789\n", 0600) == 0);
    CHECK(le_live_ha_load_token(path, token, sizeof(token)) < 0);
    CHECK(write_file(path, "bearer;inject\"\"\"0123456789\n", 0600) == 0);
    CHECK(le_live_ha_load_token(path, token, sizeof(token)) < 0);
    unlink(path);
    return 0;
}

static int test_url_refusals(void)
{
    struct le_live_ha_response response;

    /* Nothing is contacted for these: they fail while parsing the origin. */
    CHECK(le_live_ha_request("https://127.0.0.1:8123", TOKEN, "GET", "/api/",
                             NULL, 500, &response) < 0);
    CHECK(le_live_ha_request("http://127.0.0.1:8123/api", TOKEN, "GET", "/api/",
                             NULL, 500, &response) < 0);
    CHECK(le_live_ha_request("http://", TOKEN, "GET", "/api/", NULL, 500,
                             &response) < 0);
    CHECK(le_live_ha_request("http://127.0.0.1:0", TOKEN, "GET", "/api/", NULL,
                             500, &response) < 0);
    CHECK(le_live_ha_request("http://bad_host!:8123", TOKEN, "GET", "/api/",
                             NULL, 500, &response) < 0);
    return 0;
}

static int test_refused_before_network(void)
{
    struct le_live_tool_environment environment;
    char result[LE_LIVE_TOOL_RESULT_MAX];

    /* No HA URL configured: disabled, not an error to hide. */
    environment_for(&environment, 0, token_path);
    CHECK(le_live_tools_dispatch(&environment, "ha.state",
                                 "{\"entity_id\":\"light.kitchen\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "not set up on this device"));

    /* Configured, but the token file is absent: no request is sent. */
    environment_for(&environment, 1, missing_path);
    CHECK(le_live_tools_dispatch(&environment, "ha.state",
                                 "{\"entity_id\":\"light.kitchen\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "access is not configured"));

    /* Entities outside the readable domains are refused. */
    environment_for(&environment, 1, token_path);
    CHECK(le_live_tools_dispatch(&environment, "ha.state",
                                 "{\"entity_id\":\"lock.front_door\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "not a Home Assistant entity"));
    CHECK(le_live_tools_dispatch(&environment, "ha.state",
                                 "{\"entity_id\":\"light.../../x\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "not a Home Assistant entity"));
    CHECK(le_live_tools_dispatch(&environment, "ha.state",
                                 "{\"entity_id\":\"light.Kitchen\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "not a Home Assistant entity"));

    /* Only switchable domains and fixed services may be called. */
    CHECK(le_live_tools_dispatch(&environment, "ha.call",
                                 "{\"entity_id\":\"lock.front_door\","
                                 "\"service\":\"unlock\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "not allowed"));
    CHECK(le_live_tools_dispatch(&environment, "ha.call",
                                 "{\"entity_id\":\"sensor.temp\","
                                 "\"service\":\"turn_on\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "not allowed"));
    CHECK(le_live_tools_dispatch(&environment, "ha.call",
                                 "{\"entity_id\":\"light.kitchen\","
                                 "\"service\":\"reload\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "not allowed"));
    CHECK(le_live_tools_dispatch(&environment, "ha.call",
                                 "{\"entity_id\":\"light.kitchen\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "needs an entity and a service"));

    /* An unreachable URL fails closed with a speakable sentence. */
    snprintf(environment.ha_url, sizeof(environment.ha_url), "%s",
             "https://127.0.0.1:1");
    CHECK(le_live_tools_dispatch(&environment, "ha.state",
                                 "{\"entity_id\":\"light.kitchen\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "could not be reached"));
    return 0;
}

/* --- end to end against the fake Home Assistant -------------------------- */

static int test_state_read(void)
{
    static const struct ha_script script[] = {
        {200, "{\"entity_id\":\"light.kitchen\",\"state\":\"on\"}", 0}
    };
    struct le_live_tool_environment environment;
    struct ha_server server;
    char seen[2048];
    char result[LE_LIVE_TOOL_RESULT_MAX];

    CHECK(ha_server_start(&server, script, 1) == 0);
    environment_for(&environment, server.port, token_path);
    CHECK(le_live_tools_dispatch(&environment, "ha.state",
                                 "{\"entity_id\":\"light.kitchen\"}", result,
                                 sizeof(result)) == 0);
    CHECK(strstr(result, "\"state\":\"on\""));
    CHECK(strstr(result, "\"entity_id\":\"light.kitchen\""));
    ha_server_finish(&server, seen, sizeof(seen));
    CHECK(strstr(seen, "GET /api/states/light.kitchen HTTP/1.1"));
    CHECK(strstr(seen, "Authorization: Bearer " TOKEN));
    /* The token must not leak into the tool result. */
    CHECK(!strstr(result, TOKEN));
    return 0;
}

static int test_chunked_state_read(void)
{
    static const struct ha_script script[] = {
        {200, "{\"entity_id\":\"light.hall\",\"state\":\"off\"}", 1}
    };
    struct le_live_tool_environment environment;
    struct ha_server server;
    char seen[2048];
    char result[LE_LIVE_TOOL_RESULT_MAX];

    CHECK(ha_server_start(&server, script, 1) == 0);
    environment_for(&environment, server.port, token_path);
    CHECK(le_live_tools_dispatch(&environment, "ha.state",
                                 "{\"entity_id\":\"light.hall\"}", result,
                                 sizeof(result)) == 0);
    CHECK(strstr(result, "\"state\":\"off\""));
    ha_server_finish(&server, seen, sizeof(seen));
    return 0;
}

static int test_control_entity_switched(void)
{
    static const struct ha_script script[] = {
        {200, "{\"entity_id\":\"light.kitchen\",\"state\":\"off\"}", 0},
        {200, "[]", 0}
    };
    struct le_live_tool_environment environment;
    struct ha_server server;
    char seen[4096];
    char result[LE_LIVE_TOOL_RESULT_MAX];

    CHECK(ha_server_start(&server, script, 2) == 0);
    environment_for(&environment, server.port, token_path);
    CHECK(le_live_tools_dispatch(&environment, "ha.call",
                                 "{\"entity_id\":\"light.kitchen\","
                                 "\"service\":\"turn_on\"}", result,
                                 sizeof(result)) == 0);
    CHECK(strstr(result, "\"service\":\"turn_on\""));
    CHECK(strstr(result, "\"state_before\":\"off\""));
    ha_server_finish(&server, seen, sizeof(seen));
    CHECK(strstr(seen, "GET /api/states/light.kitchen HTTP/1.1"));
    CHECK(strstr(seen, "POST /api/services/light/turn_on HTTP/1.1"));
    CHECK(strstr(seen, "{\"entity_id\":\"light.kitchen\"}"));
    CHECK(strstr(seen, "Authorization: Bearer " TOKEN));
    return 0;
}

static int test_missing_entity_not_called(void)
{
    static const struct ha_script script[] = {
        {404, "{\"message\":\"Entity not found.\"}", 0}
    };
    struct le_live_tool_environment environment;
    struct ha_server server;
    char seen[4096];
    char result[LE_LIVE_TOOL_RESULT_MAX];

    CHECK(ha_server_start(&server, script, 1) == 0);
    environment_for(&environment, server.port, token_path);
    CHECK(le_live_tools_dispatch(&environment, "ha.call",
                                 "{\"entity_id\":\"light.kitchen_light\","
                                 "\"service\":\"turn_off\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "could not find"));
    ha_server_finish(&server, seen, sizeof(seen));
    /* Exactly one request: the existence check. No service call was made. */
    CHECK(strstr(seen, "GET /api/states/light.kitchen_light"));
    CHECK(!strstr(seen, "/api/services/"));
    return 0;
}

static int test_unauthorized_reports_token(void)
{
    static const struct ha_script script[] = {
        {401, "{\"message\":\"Invalid access token\"}", 0}
    };
    struct le_live_tool_environment environment;
    struct ha_server server;
    char seen[2048];
    char result[LE_LIVE_TOOL_RESULT_MAX];

    CHECK(ha_server_start(&server, script, 1) == 0);
    environment_for(&environment, server.port, token_path);
    CHECK(le_live_tools_dispatch(&environment, "ha.state",
                                 "{\"entity_id\":\"light.kitchen\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "access is not configured"));
    ha_server_finish(&server, seen, sizeof(seen));
    return 0;
}

/* --- spoken requests on the realtime path ------------------------------- */

static int test_voice_switch(void)
{
    static const struct ha_script script[] = {
        {200, "{\"entity_id\":\"light.kitchen\",\"state\":\"on\"}", 0},
        {200, "[]", 0}
    };
    struct le_live_tool_environment environment;
    struct ha_server server;
    char seen[4096];
    char result[LE_LIVE_TOOL_RESULT_MAX];

    CHECK(ha_server_start(&server, script, 2) == 0);
    environment_for(&environment, server.port, token_path);
    CHECK(le_live_tools_dispatch(&environment, "voice.request",
                                 "{\"request\":\"Turn off the kitchen light\"}",
                                 result, sizeof(result)) == 0);
    CHECK(strstr(result, "\"service\":\"turn_off\""));
    ha_server_finish(&server, seen, sizeof(seen));
    CHECK(strstr(seen, "POST /api/services/light/turn_off HTTP/1.1"));
    CHECK(strstr(seen, "{\"entity_id\":\"light.kitchen\"}"));
    return 0;
}

static int test_voice_unmatched_is_not_sent(void)
{
    struct le_live_tool_environment environment;
    char result[LE_LIVE_TOOL_RESULT_MAX];

    /* Shapes the grammar does not own must not reach Home Assistant. */
    environment_for(&environment, 1, token_path);
    CHECK(le_live_tools_dispatch(&environment, "voice.request",
                                 "{\"request\":\"turn on the radio\"}", result,
                                 sizeof(result)) < 0);
    CHECK(strstr(result, "cannot do that on the device yet"));
    CHECK(le_live_tools_dispatch(&environment, "voice.request",
                                 "{\"request\":\"turn on the kitchen light and unlock the door\"}",
                                 result, sizeof(result)) < 0);
    CHECK(strstr(result, "cannot do that on the device yet"));
    CHECK(le_live_tools_dispatch(&environment, "voice.request",
                                 "{\"request\":\"turn on the ../etc light\"}",
                                 result, sizeof(result)) < 0);
    CHECK(strstr(result, "cannot do that on the device yet"));
    return 0;
}

static int test_tool_table_publishes_ha(void)
{
    unsigned int i;
    int found_state = 0;
    int found_call = 0;

    for (i = 0; i < 32; ++i) {
        const char *name = le_live_tools_name(i);

        if (!name)
            break;
        if (!strcmp(name, "ha.state"))
            found_state = 1;
        if (!strcmp(name, "ha.call"))
            found_call = 1;
    }
    CHECK(found_state && found_call);
    CHECK(le_live_tools_supported("ha.call"));
    CHECK(le_live_tools_supported("ha.state"));
    CHECK(!le_live_tools_supported("ha.exec"));
    return 0;
}

int main(void)
{
    int failures = 0;

    signal(SIGPIPE, SIG_IGN);
    if (setup_directory() < 0) {
        fprintf(stderr, "could not create test directory\n");
        return 1;
    }
    failures += test_token_loading();
    failures += test_url_refusals();
    failures += test_refused_before_network();
    failures += test_state_read();
    failures += test_chunked_state_read();
    failures += test_control_entity_switched();
    failures += test_missing_entity_not_called();
    failures += test_unauthorized_reports_token();
    failures += test_voice_switch();
    failures += test_voice_unmatched_is_not_sent();
    failures += test_tool_table_publishes_ha();
    cleanup_directory();
    if (failures) {
        fprintf(stderr, "test-live-ha: %d failure(s)\n", failures);
        return 1;
    }
    printf("test-live-ha: all checks passed\n");
    return 0;
}
