#define _POSIX_C_SOURCE 200809L

#include "wyoming_protocol.h"

#include "../json.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

static int read_all(int fd, void *buffer, size_t size)
{
    unsigned char *position = buffer;

    while (size) {
        ssize_t count = read(fd, position, size);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        position += count;
        size -= (size_t)count;
    }
    return 0;
}

static int read_line(int fd, char *line, size_t size)
{
    size_t used = 0;

    while (used + 1 < size) {
        char byte;
        ssize_t count = read(fd, &byte, 1);

        if (count < 0 && errno == EINTR)
            continue;
        if (count != 1)
            return -1;
        if (byte == '\n') {
            line[used] = '\0';
            return 0;
        }
        line[used++] = byte;
    }
    return -1;
}

int le_wyoming_send(int fd, const char *type, const char *data_json,
                    const void *payload, size_t payload_length)
{
    char header[LE_WYOMING_HEADER_MAX];
    size_t data_length = data_json ? strlen(data_json) : 0;
    int length;

    if (fd < 0 || !type || !type[0] || strlen(type) >= LE_WYOMING_TYPE_MAX ||
        (data_length && (!json_valid_object(data_json, data_length) ||
                         data_length >= LE_WYOMING_DATA_MAX)) ||
        (payload_length && !payload))
        return -1;
    if (data_length && payload_length) {
        length = snprintf(
            header, sizeof(header),
            "{\"type\":\"%s\",\"data\":%s,"
            "\"payload_length\":%zu}\n",
            type, data_json, payload_length);
    } else if (data_length) {
        length = snprintf(
            header, sizeof(header),
            "{\"type\":\"%s\",\"data\":%s}\n",
            type, data_json);
    } else if (payload_length) {
        length = snprintf(
            header, sizeof(header),
            "{\"type\":\"%s\",\"payload_length\":%zu}\n",
            type, payload_length);
    } else {
        length = snprintf(header, sizeof(header), "{\"type\":\"%s\"}\n",
                          type);
    }
    if (length < 0 || (size_t)length >= sizeof(header))
        return -1;
    if (write_all(fd, header, (size_t)length) < 0 ||
        (payload_length && write_all(fd, payload, payload_length) < 0))
        return -1;
    return 0;
}

int le_wyoming_read_header(int fd, struct le_wyoming_event *event)
{
    char header[LE_WYOMING_HEADER_MAX];
    int data_length = 0;
    int payload_length = 0;
    int result;

    if (fd < 0 || !event || read_line(fd, header, sizeof(header)) < 0 ||
        !json_valid_object(header, strlen(header)))
        return -1;
    memset(event, 0, sizeof(*event));
    strcpy(event->header, header);
    result = json_get_string(header, "type", event->type,
                             sizeof(event->type));
    if (result != 1)
        return -1;
    result = json_get_int(header, "data_length", &data_length);
    if (result < 0 || data_length < 0 || data_length >= LE_WYOMING_DATA_MAX)
        return -1;
    result = json_get_int(header, "payload_length", &payload_length);
    if (result < 0 || payload_length < 0)
        return -1;
    event->data_length = (size_t)data_length;
    event->payload_length = (size_t)payload_length;
    if (event->data_length &&
        read_all(fd, event->data, event->data_length) < 0)
        return -1;
    event->data[event->data_length] = '\0';
    return 0;
}

int le_wyoming_read_payload(int fd, void *payload, size_t payload_size,
                            const struct le_wyoming_event *event)
{
    if (!event || event->payload_length > payload_size ||
        (event->payload_length && !payload))
        return -1;
    return event->payload_length
        ? read_all(fd, payload, event->payload_length) : 0;
}
