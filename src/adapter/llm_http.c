#define _POSIX_C_SOURCE 200809L

#include "llm_http.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define STATUS_PREFIX "__LE_HTTP_STATUS__:"
#define HTTP_LINE_MAX 32768

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

static int safe_config_value(const char *value)
{
    const unsigned char *position =
        (const unsigned char *)(value ? value : "");

    for (; *position; ++position)
        if (*position < 0x20U || *position == 0x7fU ||
            *position == '"' || *position == '\\')
            return 0;
    return 1;
}

static int append_config(char *config, size_t size, size_t *used,
                         const char *format, const char *value)
{
    int count;

    if (!safe_config_value(value) || *used >= size)
        return -1;
    count = snprintf(config + *used, size - *used, format, value);
    if (count < 0 || (size_t)count >= size - *used)
        return -1;
    *used += (size_t)count;
    return 0;
}

static int build_config(const struct le_llm_http_request *request,
                        char *config, size_t size)
{
    const char *ca_bundle = getenv("LE_AGENT_CA_BUNDLE");
    size_t used = 0;

    if (!request || strcmp(request->method, "POST") ||
        !request->url[0] || !request->content_type[0] ||
        append_config(config, size, &used, "url = \"%s\"\n",
                      request->url) < 0 ||
        append_config(config, size, &used, "request = \"%s\"\n",
                      request->method) < 0 ||
        append_config(config, size, &used,
                      "header = \"Content-Type: %s\"\n",
                      request->content_type) < 0)
        return -1;
    if (request->accept_sse &&
        append_config(config, size, &used,
                      "header = \"Accept: %s\"\n",
                      "text/event-stream") < 0)
        return -1;
    if (request->authorization[0] &&
        append_config(config, size, &used,
                      "header = \"Authorization: %s\"\n",
                      request->authorization) < 0)
        return -1;
    if (request->account_id[0] &&
        append_config(config, size, &used,
                      "header = \"ChatGPT-Account-Id: %s\"\n",
                      request->account_id) < 0)
        return -1;
    if (append_config(config, size, &used,
                      "header = \"originator: %s\"\n", "libreecho") < 0 ||
        append_config(config, size, &used,
                      "user-agent = \"%s\"\n", "libreecho-agent/0.1") < 0)
        return -1;
    if (ca_bundle && ca_bundle[0]) {
        if (append_config(config, size, &used, "cacert = \"%s\"\n",
                          ca_bundle) < 0)
            return -1;
    }
    if (used >= size ||
        snprintf(config + used, size - used,
                 "silent\nshow-error\nno-buffer\n"
                 "connect-timeout = 10\nmax-time = 45\n"
                 "proto = \"=%s\"\nproto-redir = \"=%s\"\n"
                 "%s"
                 "write-out = \"\\n%s%%{http_code}\\n\"\n",
                 request->allow_insecure_http ? "http,https" : "https",
                 request->allow_insecure_http ? "http,https" : "https",
                 request->allow_insecure_http ? "" : "tlsv1.2\n",
                 STATUS_PREFIX) >= (int)(size - used))
        return -1;
    return 0;
}

static int append_body(struct le_llm_http_response *response,
                       const char *line)
{
    size_t used = strlen(response->body);
    size_t length = strlen(line);

    if (used + length + 2 > sizeof(response->body))
        return -1;
    memcpy(response->body + used, line, length);
    used += length;
    response->body[used++] = '\n';
    response->body[used] = '\0';
    return 0;
}

static int process_line(const struct le_llm_http_request *request,
                        char *line, le_llm_http_event_fn event_fn,
                        void *event_context,
                        struct le_llm_http_response *response)
{
    size_t length = strlen(line);

    while (length &&
           (line[length - 1] == '\n' || line[length - 1] == '\r'))
        line[--length] = '\0';
    if (!strncmp(line, STATUS_PREFIX, strlen(STATUS_PREFIX))) {
        char *end;
        long status = strtol(line + strlen(STATUS_PREFIX), &end, 10);

        if (*end || status < 0 || status > 999)
            return -1;
        response->status = (int)status;
        return 0;
    }
    if (request->accept_sse && !strncmp(line, "data:", 5)) {
        const char *data = line + 5;

        if (*data == ' ')
            ++data;
        if (event_fn && event_fn(event_context, data) != 0)
            return -1;
        return 0;
    }
    if (request->accept_sse) {
        if (!line[0] || !strncmp(line, "event:", 6) ||
            !strncmp(line, "id:", 3) || !strncmp(line, "retry:", 6) ||
            line[0] == ':')
            return 0;
        return append_body(response, line);
    }
    if (!request->accept_sse && line[0])
        return append_body(response, line);
    return 0;
}

static int read_output(int fd,
                       const struct le_llm_http_request *request,
                       le_llm_http_event_fn event_fn,
                       void *event_context,
                       struct le_llm_http_response *response)
{
    char input[4096];
    char line[HTTP_LINE_MAX];
    size_t used = 0;

    for (;;) {
        ssize_t count = read(fd, input, sizeof(input));
        size_t offset = 0;

        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return -1;
        if (count == 0)
            break;
        while (offset < (size_t)count) {
            char c = input[offset++];

            if (used + 1 >= sizeof(line))
                return -1;
            line[used++] = c;
            if (c == '\n') {
                line[used] = '\0';
                if (process_line(request, line, event_fn, event_context,
                                 response) < 0)
                    return -1;
                used = 0;
            }
        }
    }
    if (used) {
        line[used] = '\0';
        if (process_line(request, line, event_fn, event_context,
                         response) < 0)
            return -1;
    }
    return 0;
}

int le_llm_http_execute(const char *curl_path,
                        const struct le_llm_http_request *request,
                        le_llm_http_event_fn event_fn,
                        void *event_context,
                        struct le_llm_http_response *response)
{
    char config[LE_LLM_TOKEN_MAX + 2048];
    int config_pipe[2] = {-1, -1};
    int body_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    pid_t child = -1;
    int child_status = 0;
    int result = -1;

    if (!curl_path || !curl_path[0] || !request || !response ||
        build_config(request, config, sizeof(config)) < 0 ||
        pipe(config_pipe) < 0 || pipe(body_pipe) < 0 ||
        pipe(output_pipe) < 0)
        goto cleanup;
    memset(response, 0, sizeof(*response));
    child = fork();
    if (child < 0)
        goto cleanup;
    if (child == 0) {
        char config_path[64];
        int null_fd;

        signal(SIGPIPE, SIG_DFL);
        close(config_pipe[1]);
        close(body_pipe[1]);
        close(output_pipe[0]);
        if (dup2(body_pipe[0], STDIN_FILENO) < 0 ||
            dup2(output_pipe[1], STDOUT_FILENO) < 0)
            _exit(126);
        null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        close(body_pipe[0]);
        close(output_pipe[1]);
        snprintf(config_path, sizeof(config_path),
                 "/proc/self/fd/%d", config_pipe[0]);
        execl(curl_path, curl_path, "--config", config_path,
              "--data-binary", "@-", (char *)NULL);
        _exit(127);
    }
    close(config_pipe[0]);
    config_pipe[0] = -1;
    close(body_pipe[0]);
    body_pipe[0] = -1;
    close(output_pipe[1]);
    output_pipe[1] = -1;
    if (write_all(config_pipe[1], config, strlen(config)) < 0 ||
        write_all(body_pipe[1], request->body,
                  strlen(request->body)) < 0)
        goto cleanup;
    close(config_pipe[1]);
    config_pipe[1] = -1;
    close(body_pipe[1]);
    body_pipe[1] = -1;
    if (read_output(output_pipe[0], request, event_fn, event_context,
                    response) < 0)
        goto cleanup;
    close(output_pipe[0]);
    output_pipe[0] = -1;
    if (request->accept_sse && response->body[0] && event_fn &&
        event_fn(event_context, response->body) != 0)
        goto cleanup;
    while (waitpid(child, &child_status, 0) < 0) {
        if (errno != EINTR)
            goto cleanup;
    }
    child = -1;
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0 ||
        response->status == 0)
        goto cleanup;
    result = 0;

cleanup:
    if (config_pipe[0] >= 0) close(config_pipe[0]);
    if (config_pipe[1] >= 0) close(config_pipe[1]);
    if (body_pipe[0] >= 0) close(body_pipe[0]);
    if (body_pipe[1] >= 0) close(body_pipe[1]);
    if (output_pipe[0] >= 0) close(output_pipe[0]);
    if (output_pipe[1] >= 0) close(output_pipe[1]);
    if (child > 0) {
        kill(child, SIGTERM);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    memset(config, 0, sizeof(config));
    return result;
}
