#define _POSIX_C_SOURCE 200809L

#include "live_ha.h"
#include "live_dns.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define HA_DEFAULT_PORT 8123U
#define HA_HOST_MAX 127U
#define HA_REQUEST_MAX 1024U
#define HA_RAW_MAX (LE_LIVE_HA_BODY_MAX * 2U + 512U)

static int token_byte_ok(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

int le_live_ha_load_token(const char *path, char *token, size_t size)
{
    char raw[LE_LIVE_HA_TOKEN_MAX + 1];
    ssize_t count;
    size_t length;
    size_t i;
    int fd;

    if (token && size)
        token[0] = '\0';
    if (!path || !token || size < LE_LIVE_HA_TOKEN_MAX)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    count = read(fd, raw, sizeof(raw) - 1U);
    close(fd);
    if (count <= 0)
        return -1;
    raw[count] = '\0';
    length = (size_t)count;
    while (length > 0 && (raw[length - 1U] == '\n' || raw[length - 1U] == '\r' ||
                          raw[length - 1U] == ' ' || raw[length - 1U] == '\t'))
        raw[--length] = '\0';
    if (length < 16U || length > LE_LIVE_HA_TOKEN_MAX - 1U)
        return -1;
    for (i = 0; i < length; ++i)
        if (!token_byte_ok((unsigned char)raw[i]))
            return -1;
    memcpy(token, raw, length + 1U);
    memset(raw, 0, sizeof(raw));
    return 0;
}

/* Split "http://host[:port]" into host and port.  Origin only, no path. */
static int parse_origin(const char *base_url, char *host, size_t host_size,
                        unsigned int *port)
{
    const char *rest;
    const char *colon;
    size_t host_length;
    size_t i;

    *port = HA_DEFAULT_PORT;
    if (!base_url || strncmp(base_url, "http://", 7) != 0)
        return -1;
    rest = base_url + 7;
    colon = strchr(rest, ':');
    host_length = strcspn(rest, ":/");
    if (!host_length || host_length > HA_HOST_MAX || host_length + 1U > host_size)
        return -1;
    for (i = 0; i < host_length; ++i) {
        unsigned char c = (unsigned char)rest[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return -1;
    }
    memcpy(host, rest, host_length);
    host[host_length] = '\0';
    if (colon && colon == rest + host_length) {
        char *end = NULL;
        unsigned long value = strtoul(colon + 1, &end, 10);

        if (end == colon + 1 || value == 0UL || value > 65535UL)
            return -1;
        *port = (unsigned int)value;
        rest = end;
    } else {
        rest = rest + host_length;
    }
    if (*rest == '/')
        ++rest;
    return *rest == '\0' ? 0 : -1;
}

static int connect_to(const char *host, unsigned int port, int timeout_ms)
{
    struct sockaddr_in endpoint;
    struct timeval deadline;
    int fd;

    memset(&endpoint, 0, sizeof(endpoint));
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &endpoint.sin_addr) != 1 &&
        le_live_dns_resolve_ipv4(host, NULL, &endpoint.sin_addr, timeout_ms) < 0)
        return -1;
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    /* SO_SNDTIMEO bounds connect() as well as send() on Linux. */
    deadline.tv_sec = timeout_ms / 1000;
    deadline.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline)) < 0 ||
        connect(fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t wrote = send(fd, data, length, MSG_NOSIGNAL);

        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            return -1;
        data += wrote;
        length -= (size_t)wrote;
    }
    return 0;
}

/* Case-insensitive header presence check inside the header block. */
static int header_contains(const char *headers, const char *name,
                           const char *value)
{
    const char *line = headers;

    while (line && *line) {
        size_t name_length = strlen(name);

        if (!strncasecmp(line, name, name_length)) {
            const char *v = line + name_length;

            while (*v == ' ' || *v == '\t')
                ++v;
            if (!strncasecmp(v, value, strlen(value)))
                return 1;
        }
        line = strstr(line, "\r\n");
        if (line)
            line += 2;
    }
    return 0;
}

/*
 * Decode a chunked body in place into `out`.  Returns the decoded length, or
 * -1 for malformed framing or a body larger than `out`.
 */
static long dechunk(const char *in, size_t in_length, char *out, size_t out_size)
{
    size_t used = 0;
    size_t pos = 0;

    for (;;) {
        char *end = NULL;
        unsigned long chunk;
        const char *line_end;

        if (pos >= in_length)
            return -1;
        line_end = memchr(in + pos, '\n', in_length - pos);
        if (!line_end)
            return -1;
        chunk = strtoul(in + pos, &end, 16);
        if (end == in + pos || chunk > in_length)
            return -1;
        pos = (size_t)(line_end - in) + 1U;
        if (chunk == 0UL)
            return (long)used;
        if (pos + chunk > in_length || used + chunk >= out_size)
            return -1;
        memcpy(out + used, in + pos, (size_t)chunk);
        used += (size_t)chunk;
        pos += (size_t)chunk;
        /* Skip the CRLF that terminates each chunk. */
        if (pos < in_length && in[pos] == '\r')
            ++pos;
        if (pos < in_length && in[pos] == '\n')
            ++pos;
    }
}

int le_live_ha_request(const char *base_url, const char *token,
                       const char *method, const char *path, const char *body,
                       int timeout_ms, struct le_live_ha_response *response)
{
    char host[HA_HOST_MAX + 1U];
    char request[HA_REQUEST_MAX];
    char raw[HA_RAW_MAX];
    char decoded[LE_LIVE_HA_BODY_MAX];
    unsigned int port = HA_DEFAULT_PORT;
    size_t raw_used = 0;
    size_t body_length = body ? strlen(body) : 0U;
    const char *header_end;
    const char *payload;
    size_t payload_length;
    int written;
    int fd;

    if (!response || !method || !path || timeout_ms <= 0)
        return -1;
    memset(response, 0, sizeof(*response));
    if (parse_origin(base_url, host, sizeof(host), &port) < 0)
        return -1;
    written = snprintf(request, sizeof(request),
                       "%s %s HTTP/1.1\r\nHost: %s:%u\r\n"
                       "Accept: application/json\r\nConnection: close\r\n%s%s"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %lu\r\n\r\n%s",
                       method, path, host, port,
                       token && token[0] ? "Authorization: Bearer " : "",
                       token && token[0] ? token : "",
                       (unsigned long)body_length, body ? body : "");
    if (written < 0 || (size_t)written >= sizeof(request))
        return -1;
    /* The token is spliced into a header line, so it must not carry a control
       byte or a separator that could end the header early. */
    if (token) {
        size_t i;

        for (i = 0; token[i]; ++i)
            if (!token_byte_ok((unsigned char)token[i]))
                return -1;
    }

    fd = connect_to(host, port, timeout_ms);
    if (fd < 0)
        return -1;
    if (send_all(fd, request, (size_t)written) < 0) {
        close(fd);
        return -1;
    }
    for (;;) {
        ssize_t got;

        if (raw_used >= sizeof(raw) - 1U) {
            close(fd);
            return -1;
        }
        got = recv(fd, raw + raw_used, sizeof(raw) - 1U - raw_used, 0);
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            close(fd);
            return -1;
        }
        if (got == 0)
            break;
        raw_used += (size_t)got;
    }
    close(fd);
    raw[raw_used] = '\0';

    if (raw_used < 12U || strncmp(raw, "HTTP/1.", 7) != 0)
        return -1;
    response->status = atoi(raw + 9);
    header_end = strstr(raw, "\r\n\r\n");
    if (!header_end)
        return -1;
    payload = header_end + 4;
    payload_length = raw_used - (size_t)(payload - raw);

    if (header_contains(raw, "Transfer-Encoding:", "chunked")) {
        long count = dechunk(payload, payload_length, decoded, sizeof(decoded));

        if (count < 0)
            return -1;
        payload = decoded;
        payload_length = (size_t)count;
    }
    if (payload_length >= sizeof(response->body))
        return -1;
    memcpy(response->body, payload, payload_length);
    response->body[payload_length] = '\0';
    response->length = payload_length;
    return 0;
}
