#ifndef LIBREECHO_WYOMING_PROTOCOL_H
#define LIBREECHO_WYOMING_PROTOCOL_H

#include <stddef.h>

#define LE_WYOMING_TYPE_MAX 64
#define LE_WYOMING_HEADER_MAX 4096
#define LE_WYOMING_DATA_MAX 2048

struct le_wyoming_event {
    char type[LE_WYOMING_TYPE_MAX];
    char header[LE_WYOMING_HEADER_MAX];
    char data[LE_WYOMING_DATA_MAX];
    size_t data_length;
    size_t payload_length;
};

/* Send one Wyoming JSONL event, followed by optional data and binary payload. */
int le_wyoming_send(int fd, const char *type, const char *data_json,
                    const void *payload, size_t payload_length);

/* Read one event header and its optional JSON data; payload is not consumed. */
int le_wyoming_read_header(int fd, struct le_wyoming_event *event);

/* Read exactly the payload advertised by the most recent event. */
int le_wyoming_read_payload(int fd, void *payload, size_t payload_size,
                            const struct le_wyoming_event *event);

#endif
