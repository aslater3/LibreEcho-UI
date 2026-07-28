#define _POSIX_C_SOURCE 200809L

#include "adapter/wyoming_protocol.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    int sockets[2];
    const unsigned char payload[] = {0, 1, 2, 3, 4, 5};
    unsigned char received[sizeof(payload)];
    struct le_wyoming_event event;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    CHECK(le_wyoming_send(
              sockets[0], "audio-chunk",
              "{\"rate\":16000,\"width\":2,\"channels\":1}",
              payload, sizeof(payload)) == 0);
    CHECK(le_wyoming_read_header(sockets[1], &event) == 0);
    CHECK(!strcmp(event.type, "audio-chunk"));
    CHECK(strstr(event.header, "\"data_length\":") != NULL);
    CHECK(strstr(event.data, "\"rate\":16000") != NULL);
    CHECK(event.payload_length == sizeof(payload));
    CHECK(le_wyoming_read_payload(
              sockets[1], received, sizeof(received), &event) == 0);
    CHECK(!memcmp(received, payload, sizeof(payload)));
    close(sockets[0]);
    close(sockets[1]);
    puts("wyoming protocol: framing and PCM payload: ok");
    return 0;
}
