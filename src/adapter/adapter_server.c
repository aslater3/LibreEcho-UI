#include "adapter.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

/* sys/stat.h is intentionally not needed by this small protocol helper. */
extern int chmod(const char *path, unsigned int mode);

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

static const char *json_value_end(const char *p)
{
    char stack[LE_ADAPTER_MSG_MAX];
    size_t depth = 0;
    const char *q;

    if (!p || (*p != '{' && *p != '['))
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


int le_adapter_listen(const char *sock_path)
{
    struct sockaddr_un address;
    socklen_t address_len;
    int fd;
    size_t path_len;

    if (!sock_path)
        return -1;
    path_len = strlen(sock_path);
    if (path_len == 0 || path_len >= sizeof(address.sun_path))
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, sock_path, path_len + 1);
    address_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                              path_len + 1);
    unlink(sock_path);
    if (bind(fd, (struct sockaddr *)&address, address_len) < 0 ||
        listen(fd, 4) < 0) {
        int saved_errno = errno;

        close(fd);
        unlink(sock_path);
        errno = saved_errno;
        return -1;
    }
    if (chmod(sock_path, 0660) < 0) {
        int saved_errno = errno;

        close(fd);
        unlink(sock_path);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

int le_adapter_accept(int listen_fd)
{
    if (listen_fd < 0)
        return -1;
    return accept(listen_fd, NULL, NULL);
}

int le_adapter_parse_request(char *msg, char *cmd, size_t cmd_size,
                             char **args, unsigned long *id)
{
    const char *root_end;
    const char *value;
    const char *cmd_end;
    const char *args_end;
    unsigned long version;
    char *args_value;

    if (!msg || !cmd || cmd_size == 0 || !args || !id)
        return -1;
    root_end = json_value_end(msg);
    if (!root_end || *skip_ws(root_end) != '\0')
        return -1;
    value = find_key(msg, "v");
    if (parse_ulong_value(value, &version) < 0 ||
        version != LE_ADAPTER_PROTO_VERSION)
        return -1;
    value = find_key(msg, "id");
    if (parse_ulong_value(value, id) < 0)
        return -1;
    value = find_key(msg, "cmd");
    if (json_read_string(value, cmd, cmd_size, &cmd_end) < 0)
        return -1;
    cmd_end = skip_ws(cmd_end);
    if (*cmd_end != ',')
        return -1;
    args_value = (char *)find_key(msg, "args");
    if (!args_value || *args_value != '{')
        return -1;
    args_end = json_value_end(args_value);
    if (!args_end)
        return -1;
    args_end = skip_ws(args_end);
    if (*args_end != ',' && *args_end != '}')
        return -1;
    ((char *)args_end)[0] = '\0';
    *args = args_value;
    return 0;
}

int le_adapter_respond_ok(char *buf, size_t size, unsigned long id,
                          const char *data_json)
{
    int n;

    if (!buf || size == 0)
        return -1;
    if (!data_json)
        data_json = "{}";
    n = snprintf(buf, size,
                 "{\"v\":1,\"id\":%lu,\"ok\":true,\"data\":%s}\n",
                 id, data_json);
    if (n < 0 || (size_t)n >= size)
        return -1;
    return n;
}

int le_adapter_respond_err(char *buf, size_t size, unsigned long id,
                           const char *error_msg)
{
    size_t used;
    int n;

    if (!buf || size == 0)
        return -1;
    n = snprintf(buf, size, "{\"v\":1,\"id\":%lu,\"ok\":false,\"error\":\"",
                 id);
    if (n < 0 || (size_t)n >= size)
        return -1;
    used = (size_t)n;
    if (append_json_string(buf, size, &used, error_msg) < 0)
        return -1;
    n = snprintf(buf + used, size - used, "\"}\n");
    if (n < 0 || (size_t)n >= size - used)
        return -1;
    return (int)(used + (size_t)n);
}

int le_adapter_format_event(char *buf, size_t size, const char *event_type,
                            const char *data_json)
{
    size_t used;
    int n;

    if (!buf || size == 0 || !event_type)
        return -1;
    if (!data_json)
        data_json = "{}";
    n = snprintf(buf, size, "{\"v\":1,\"event\":\"");
    if (n < 0 || (size_t)n >= size)
        return -1;
    used = (size_t)n;
    if (append_json_string(buf, size, &used, event_type) < 0)
        return -1;
    n = snprintf(buf + used, size - used, "\",\"data\":%s}\n", data_json);
    if (n < 0 || (size_t)n >= size - used)
        return -1;
    return (int)(used + (size_t)n);
}
