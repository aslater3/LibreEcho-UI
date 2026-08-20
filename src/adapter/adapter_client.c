#include "adapter.h"
#include "log.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

struct le_adapter {
    int fd;
    unsigned long next_id;
};

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    return p;
}

static const char *find_key(const char *json, const char *key)
{
    char needle[64];
    size_t key_len;
    const char *p;
    int n;

    if (!json || !key)
        return NULL;
    n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return NULL;
    key_len = (size_t)n;
    p = json;
    while ((p = strstr(p, needle)) != NULL) {
        const char *value = skip_ws(p + key_len);
        if (*value == ':')
            return skip_ws(value + 1);
        p += key_len;
    }
    return NULL;
}

static int parse_ulong_value(const char *p, unsigned long *value)
{
    char *end;
    unsigned long v;

    if (!p || !value || *p < '0' || *p > '9')
        return -1;
    errno = 0;
    v = strtoul(p, &end, 10);
    if (end == p || errno == ERANGE)
        return -1;
    end = (char *)skip_ws(end);
    if (*end != ',' && *end != '}')
        return -1;
    *value = v;
    return 0;
}

static int parse_bool_value(const char *p, int *value)
{
    if (!p || !value)
        return -1;
    if (!strncmp(p, "true", 4) &&
        (p[4] == ',' || p[4] == '}' || p[4] == ']' || p[4] == '\0')) {
        *value = 1;
        return 0;
    }
    if (!strncmp(p, "false", 5) &&
        (p[5] == ',' || p[5] == '}' || p[5] == ']' || p[5] == '\0')) {
        *value = 0;
        return 0;
    }
    return -1;
}

static const char *json_value_end(const char *p)
{
    char stack[LE_ADAPTER_MSG_MAX];
    size_t depth = 0;
    const char *q;

    if (!p)
        return NULL;
    if (*p != '{' && *p != '[')
        return NULL;
    q = p;
    while (*q) {
        if (*q == '"') {
            ++q;
            while (*q) {
                if (*q == '\\' && q[1] != '\0') {
                    q += 2;
                    continue;
                }
                if (*q == '"') {
                    ++q;
                    break;
                }
                ++q;
            }
            continue;
        }
        if (*q == '{' || *q == '[') {
            if (depth >= sizeof(stack))
                return NULL;
            stack[depth++] = *q;
        } else if (*q == '}' || *q == ']') {
            if (depth == 0 ||
                (stack[depth - 1] == '{' && *q != '}') ||
                (stack[depth - 1] == '[' && *q != ']'))
                return NULL;
            --depth;
            if (depth == 0)
                return q + 1;
        }
        ++q;
    }
    return NULL;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Return 0 for success, -1 for malformed JSON, -2 for a small output. */
static int json_read_string(const char *p, char *out, size_t out_size,
                            const char **end_out)
{
    size_t used = 0;
    const char *q;

    if (!p || *p != '"')
        return -1;
    q = p + 1;
    while (*q && *q != '"') {
        unsigned char c = (unsigned char)*q++;
        if (c == '\\') {
            int h;
            if (*q == '\0')
                return -1;
            switch (*q++) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
                h = hex_value(q[0]);
                if (h < 0 || hex_value(q[1]) < 0 ||
                    hex_value(q[2]) < 0 || hex_value(q[3]) < 0)
                    return -1;
                c = (unsigned char)((h << 4) | hex_value(q[1]));
                /* ASCII command/error text is the protocol's common case. */
                q += 4;
                if (c == 0)
                    return -1;
                break;
            default:
                return -1;
            }
        } else if (c < 0x20) {
            return -1;
        }
        if (out) {
            if (used + 1 >= out_size)
                return -2;
            out[used] = (char)c;
        }
        ++used;
    }
    if (*q != '"')
        return -1;
    if (out)
        out[used] = '\0';
    if (end_out)
        *end_out = q + 1;
    return 0;
}

static int append_json_string(char *buf, size_t size, size_t *used,
                              const char *text)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)(text ? text : "");

    while (*p) {
        unsigned char c = *p++;
        char escaped[7];
        size_t n = 1;

        if (c == '"' || c == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)c;
            n = 2;
        } else if (c == '\b' || c == '\f' || c == '\n' ||
                   c == '\r' || c == '\t') {
            escaped[0] = '\\';
            escaped[1] = c == '\b' ? 'b' : c == '\f' ? 'f' :
                         c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
            n = 2;
        } else if (c < 0x20) {
            escaped[0] = '\\';
            escaped[1] = 'u';
            escaped[2] = '0';
            escaped[3] = '0';
            escaped[4] = hex[c >> 4];
            escaped[5] = hex[c & 0x0f];
            n = 6;
        } else {
            escaped[0] = (char)c;
        }
        if (*used > size || n > size - *used - 1)
            return -1;
        memcpy(buf + *used, escaped, n);
        *used += n;
    }
    if (*used >= size)
        return -1;
    buf[*used] = '\0';
    return 0;
}

static int wait_for_fd(int fd, short events, int timeout_ms)
{
    struct pollfd pfd;
    int rc;

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    if (rc == 0)
        return LE_ADAPTER_ERR_TIMEOUT;
    if (rc < 0 || (pfd.revents & POLLNVAL) ||
        ((pfd.revents & (POLLERR | POLLHUP)) &&
         !(pfd.revents & events)))
        return LE_ADAPTER_ERR_IO;
    return 0;
}

static ssize_t adapter_send(int fd, const void *buf, size_t size)
{
#ifdef MSG_NOSIGNAL
    return send(fd, buf, size, MSG_NOSIGNAL);
#else
    return write(fd, buf, size);
#endif
}

static int write_all(struct le_adapter *a, const char *buf, size_t size)
{
    size_t sent = 0;

    while (sent < size) {
        ssize_t n = adapter_send(a->fd, buf + sent, size - sent);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int rc = wait_for_fd(a->fd, POLLOUT, 5000);
            if (rc)
                return rc;
            continue;
        }
        return LE_ADAPTER_ERR_IO;
    }
    return LE_ADAPTER_OK;
}

static int read_line(struct le_adapter *a, char *buf, size_t size)
{
    size_t used = 0;

    if (!buf || size < 2)
        return LE_ADAPTER_ERR_PROTO;
    for (;;) {
        ssize_t n;
        size_t i;
        int rc = wait_for_fd(a->fd, POLLIN, 5000);
        if (rc)
            return rc;
        do {
            n = read(a->fd, buf + used, size - used - 1);
        } while (n < 0 && errno == EINTR);
        if (n == 0)
            return LE_ADAPTER_ERR_IO;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return LE_ADAPTER_ERR_IO;
        }
        used += (size_t)n;
        for (i = used - (size_t)n; i < used; ++i) {
            if (buf[i] == '\n') {
                buf[i] = '\0';
                return LE_ADAPTER_OK;
            }
        }
        if (used >= size - 1)
            return LE_ADAPTER_ERR_PROTO;
    }
}

struct le_adapter *le_adapter_connect(const char *sock_path, int timeout_ms)
{
    struct sockaddr_un address;
    struct le_adapter *a;
    socklen_t address_len;
    int fd;
    int flags;
    int error = 0;
    socklen_t error_size = (socklen_t)sizeof(error);
    int rc;

    if (!sock_path || timeout_ms < 0 || strlen(sock_path) >= sizeof(address.sun_path)) {
        errno = EINVAL;
        return NULL;
    }
    le_log_debug("adapter: connecting to %s (timeout %dms)", sock_path, timeout_ms);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        le_log_pdebug("adapter: socket() failed for %s", sock_path);
        return NULL;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, sock_path, strlen(sock_path) + 1);
    address_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                               strlen(sock_path) + 1);

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return NULL;
    }
    rc = connect(fd, (struct sockaddr *)&address, address_len);
    if (rc < 0) {
        if (errno != EINPROGRESS) {
            int saved_errno = errno;
            le_log_pdebug("adapter: connect(%s) failed", sock_path);
            close(fd);
            errno = saved_errno;
            return NULL;
        }
        {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            do {
                rc = poll(&pfd, 1, timeout_ms);
            } while (rc < 0 && errno == EINTR);
            if (rc <= 0 || (pfd.revents & POLLNVAL)) {
                int saved_errno = rc == 0 ? ETIMEDOUT : (rc < 0 ? errno : EBADF);
                le_log_debug("adapter: connect(%s) timed out or invalid", sock_path);
                close(fd);
                errno = saved_errno;
                return NULL;
            }
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) < 0 ||
            error != 0) {
            int saved_errno = error ? error : errno;
            close(fd);
            errno = saved_errno ? saved_errno : EIO;
            return NULL;
        }
    }
    /* Keep the descriptor non-blocking so partial I/O can use poll(). */
    a = (struct le_adapter *)malloc(sizeof(*a));
    if (!a) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    a->fd = fd;
    a->next_id = 1;
    le_log_debug("adapter: connected to %s (fd=%d)", sock_path, fd);
    return a;
}

void le_adapter_close(struct le_adapter *a)
{
    if (!a)
        return;
    close(a->fd);
    free(a);
}

int le_adapter_call(struct le_adapter *a, const char *cmd,
                    const char *args_json, char *out, size_t out_size)
{
    char request[LE_ADAPTER_MSG_MAX];
    char response[LE_ADAPTER_MSG_MAX];
    char error_text[LE_ADAPTER_MSG_MAX];
    const char *data;
    const char *data_end;
    const char *value;
    size_t used;
    size_t data_size;
    unsigned long id;
    unsigned long response_id;
    int ok;
    int n;
    int rc;

    if (out && out_size)
        out[0] = '\0';
    if (!a || a->fd < 0 || !cmd || (out_size && !out))
        return LE_ADAPTER_ERR_PROTO;
    if (a->next_id == 0)
        a->next_id = 1;
    id = a->next_id++;

    n = snprintf(request, sizeof(request),
                 "{\"v\":1,\"id\":%lu,\"cmd\":\"", id);
    if (n < 0 || (size_t)n >= sizeof(request))
        return LE_ADAPTER_ERR_PROTO;
    used = (size_t)n;
    if (append_json_string(request, sizeof(request), &used, cmd) < 0)
        return LE_ADAPTER_ERR_PROTO;
    n = snprintf(request + used, sizeof(request) - used, "\",\"args\":");
    if (n < 0 || (size_t)n >= sizeof(request) - used)
        return LE_ADAPTER_ERR_PROTO;
    used += (size_t)n;
    if (!args_json)
        args_json = "{}";
    value = skip_ws(args_json);
    if (*value != '{')
        return LE_ADAPTER_ERR_PROTO;
    data_end = json_value_end(value);
    if (!data_end || *skip_ws(data_end) != '\0')
        return LE_ADAPTER_ERR_PROTO;
    if (strlen(args_json) > sizeof(request) - used - 3)
        return LE_ADAPTER_ERR_PROTO;
    n = snprintf(request + used, sizeof(request) - used, "%s}\n", args_json);
    if (n < 0 || (size_t)n >= sizeof(request) - used)
        return LE_ADAPTER_ERR_PROTO;
    used += (size_t)n;

    le_log_debug("adapter: call id=%lu cmd=\"%s\"", id, cmd);
    rc = write_all(a, request, used);
    if (rc != LE_ADAPTER_OK) {
        le_log_debug("adapter: write failed for cmd=\"%s\" (rc=%d)", cmd, rc);
        return rc;
    }
    rc = read_line(a, response, sizeof(response));
    if (rc != LE_ADAPTER_OK) {
        le_log_debug("adapter: read failed for cmd=\"%s\" (rc=%d)", cmd, rc);
        return rc;
    }

    value = find_key(response, "v");
    if (parse_ulong_value(value, &response_id) < 0 ||
        response_id != LE_ADAPTER_PROTO_VERSION)
        return LE_ADAPTER_ERR_PROTO;
    value = find_key(response, "id");
    if (parse_ulong_value(value, &response_id) < 0 || response_id != id)
        return LE_ADAPTER_ERR_PROTO;
    value = find_key(response, "ok");
    if (parse_bool_value(value, &ok) < 0)
        return LE_ADAPTER_ERR_PROTO;
    if (!ok) {
        value = find_key(response, "error");
        if (json_read_string(value, error_text, sizeof(error_text), NULL) < 0)
            return LE_ADAPTER_ERR_PROTO;
        le_log_debug("adapter: cmd=\"%s\" rejected: %s", cmd, error_text);
        if (out && out_size) {
            size_t error_size = strlen(error_text);
            if (error_size >= out_size)
                error_size = out_size - 1;
            memcpy(out, error_text, error_size);
            out[error_size] = '\0';
        }
        return LE_ADAPTER_ERR_REJECTED;
    }

    data = find_key(response, "data");
    data_end = json_value_end(data);
    if (!data || !data_end ||
        (*skip_ws(data_end) != '}' && *skip_ws(data_end) != ','))
        return LE_ADAPTER_ERR_PROTO;
    if (out && out_size) {
        data_size = (size_t)(data_end - data);
        if (data_size >= out_size)
            return LE_ADAPTER_ERR_IO;
        memcpy(out, data, data_size);
        out[data_size] = '\0';
    }
    return LE_ADAPTER_OK;
}
