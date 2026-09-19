#define _POSIX_C_SOURCE 200809L

/*
 * Base64 and RFC 6455 framing.
 *
 * The framing is where a WebSocket client is easy to get wrong in ways that
 * only show up against a real server: masking the wrong direction, forgetting
 * the mask bit, mishandling the 7/16/64-bit length forms, or accepting a frame
 * it should refuse. All of that is checked here against a socketpair, with the
 * peer side written out by hand so the bytes are inspected rather than trusted.
 */

#include "adapter/live_b64.h"
#include "adapter/ws_client.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* --- base64 ------------------------------------------------------------- */

static int test_base64(void)
{
    char encoded[64];
    unsigned char decoded[64];
    size_t i;

    CHECK(le_b64_encode("abc", 3, encoded, sizeof(encoded)) == 4);
    CHECK(!strcmp(encoded, "YWJj"));
    CHECK(le_b64_encode("ab", 2, encoded, sizeof(encoded)) == 4);
    CHECK(!strcmp(encoded, "YWI="));
    CHECK(le_b64_encode("a", 1, encoded, sizeof(encoded)) == 4);
    CHECK(!strcmp(encoded, "YQ=="));

    CHECK(le_b64_decode("YWJj", decoded, sizeof(decoded)) == 3);
    CHECK(!memcmp(decoded, "abc", 3));
    CHECK(le_b64_decode("YWI=", decoded, sizeof(decoded)) == 2);
    CHECK(!memcmp(decoded, "ab", 2));
    CHECK(le_b64_decode("YQ==", decoded, sizeof(decoded)) == 1);
    CHECK(!memcmp(decoded, "a", 1));

    /* Every byte value survives a round trip at every alignment. */
    for (i = 1; i <= 8; ++i) {
        unsigned char raw[8];
        size_t j;

        for (j = 0; j < i; ++j)
            raw[j] = (unsigned char)(j * 37U + i);
        CHECK(le_b64_encode(raw, i, encoded, sizeof(encoded)) > 0);
        CHECK(le_b64_decode(encoded, decoded, sizeof(decoded)) == i);
        CHECK(!memcmp(raw, decoded, i));
    }
    /* Whitespace is tolerated; garbage and a bad tail are not. */
    CHECK(le_b64_decode("YW Jj\n", decoded, sizeof(decoded)) == 3);
    CHECK(le_b64_decode("YW!j", decoded, sizeof(decoded)) == 0);
    CHECK(le_b64_decode("Y", decoded, sizeof(decoded)) == 0);
    /* A destination that is too small is an error, never a truncation. */
    CHECK(le_b64_decode("YWJj", decoded, 2) == 0);
    CHECK(le_b64_encode("abc", 3, encoded, 4) == 0);
    return 0;
}

/* --- websocket framing -------------------------------------------------- */

/*
 * A socketpair is two ends, not four descriptors: whatever is written to
 * `client_fd` is read from `peer_fd` and vice versa. The client therefore uses
 * one end for both directions and the test uses the other, which is also how a
 * real connection behaves.
 */
struct peer {
    int client_fd;    /* the end le_ws reads and writes */
    int peer_fd;      /* the end this test reads and writes */
};

static long peer_recv(void *context, void *buffer, size_t length)
{
    struct peer *p = context;
    ssize_t got = recv(p->client_fd, buffer, length, 0);

    if (got > 0)
        return (long)got;
    if (got == 0)
        return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return -2;
    return -1;
}

static long peer_send(void *context, const void *buffer, size_t length)
{
    struct peer *p = context;
    ssize_t wrote = send(p->client_fd, buffer, length, MSG_NOSIGNAL);

    if (wrote > 0)
        return (long)wrote;
    if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == EINTR))
        return -2;
    return -1;
}

static long failing_recv(void *context, void *buffer, size_t length)
{
    (void)context;
    (void)buffer;
    (void)length;
    return -1;
}

/* Read exactly `length` bytes the client sent, with a bounded budget. */
static int drain(int fd, unsigned char *out, size_t length)
{
    size_t used = 0;
    int spins = 0;

    while (used < length && spins < 2000) {
        ssize_t got = read(fd, out + used, length - used);

        if (got > 0) {
            used += (size_t)got;
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ++spins;
            continue;
        }
        return -1;
    }
    return used == length ? 0 : -1;
}

static int setup(struct le_ws *ws, struct le_ws_stream *stream,
                 struct peer *peer, int pair[2])
{
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    memset(peer, 0, sizeof(*peer));
    peer->client_fd = pair[0];
    peer->peer_fd = pair[1];
    fcntl(pair[0], F_SETFL, fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK);
    memset(ws, 0, sizeof(*ws));
    stream->context = peer;
    stream->recv = peer_recv;
    stream->send = peer_send;
    ws->stream = stream;
    ws->connected = 1;
    ws->send_timeout_ms = 500;
    ws->urandom = open("/dev/urandom", O_RDONLY);
    CHECK(ws->urandom >= 0);
    return 0;
}

static int test_client_frames_are_masked(void)
{
    struct le_ws ws;
    struct le_ws_stream stream;
    struct peer peer;
    int pair[2];
    unsigned char frame[64];
    const char *payload = "{\"type\":\"session.close\"}";
    size_t length = strlen(payload);
    unsigned char key[4];
    size_t i;

    CHECK(setup(&ws, &stream, &peer, pair) == 0);
    CHECK(le_ws_send_text(&ws, payload, length) == 0);
    CHECK(drain(peer.peer_fd, frame, 2 + 4 + length) == 0);

    CHECK((frame[0] & 0x80U) != 0);            /* FIN */
    CHECK((frame[0] & 0x0fU) == 0x1);          /* text */
    CHECK((frame[1] & 0x80U) != 0);            /* MASKED: required of clients */
    CHECK((frame[1] & 0x7fU) == length);
    memcpy(key, frame + 2, 4);
    for (i = 0; i < length; ++i)
        CHECK((unsigned char)(frame[6 + i] ^ key[i % 4U]) ==
              (unsigned char)payload[i]);
    /* The caller's buffer must be untouched: masking is not an in-place edit. */
    CHECK(!strcmp(payload, "{\"type\":\"session.close\"}"));

    /* A long frame uses the 16-bit length form. */
    {
        char big[300];
        unsigned char header[8];

        memset(big, 'x', sizeof(big));
        CHECK(le_ws_send_text(&ws, big, sizeof(big)) == 0);
        CHECK(drain(peer.peer_fd, header, 4) == 0);
        CHECK((header[1] & 0x7fU) == 126U);
        CHECK((((size_t)header[2] << 8) | header[3]) == sizeof(big));
        CHECK(drain(peer.peer_fd, header, 4) == 0);   /* mask key */
        {
            unsigned char body[sizeof(big)];

            CHECK(drain(peer.peer_fd, body, sizeof(big)) == 0);
            for (i = 0; i < sizeof(big); ++i)
                CHECK((unsigned char)(body[i] ^ header[i % 4U]) == 'x');
        }
    }
    close(pair[0]);
    close(pair[1]);
    le_ws_close(&ws);
    return 0;
}

static int send_server_frame(int fd, int opcode, int fin, const void *payload,
                            size_t length)
{
    unsigned char header[10];
    size_t header_length = 2;

    header[0] = (unsigned char)((fin ? 0x80 : 0) | opcode);
    if (length < 126U) {
        header[1] = (unsigned char)length;
    } else if (length <= 0xffffU) {
        header[1] = 126U;
        header[2] = (unsigned char)(length >> 8);
        header[3] = (unsigned char)length;
        header_length = 4;
    } else {
        header[1] = 127U;
        memset(header + 2, 0, 6);
        header[8] = (unsigned char)(length >> 8);
        header[9] = (unsigned char)length;
        header_length = 10;
    }
    if (write(fd, header, header_length) != (ssize_t)header_length)
        return -1;
    if (length && write(fd, payload, length) != (ssize_t)length)
        return -1;
    return 0;
}

static int test_server_frames_are_read_and_validated(void)
{
    struct le_ws ws;
    struct le_ws_stream stream;
    struct peer peer;
    int pair[2];
    char message[256];

    CHECK(setup(&ws, &stream, &peer, pair) == 0);

    /* A plain text frame. */
    CHECK(send_server_frame(peer.peer_fd, 0x1, 1, "{\"type\":\"turn.done\"}",
                            strlen("{\"type\":\"turn.done\"}")) == 0);
    CHECK(le_ws_read_text(&ws, message, sizeof(message), 500) == 1);
    CHECK(!strcmp(message, "{\"type\":\"turn.done\"}"));
    CHECK(ws.frames_in == 1);

    /* A ping is answered without surfacing, and the next text frame is read. */
    CHECK(send_server_frame(peer.peer_fd, 0x9, 1, "hi", 2) == 0);
    CHECK(send_server_frame(peer.peer_fd, 0x1, 1, "after-ping",
                            strlen("after-ping")) == 0);
    CHECK(le_ws_read_text(&ws, message, sizeof(message), 500) == 1);
    CHECK(!strcmp(message, "after-ping"));
    {
        unsigned char frame[8];

        CHECK(drain(peer.peer_fd, frame, 2 + 4 + 2) == 0);
        CHECK((frame[0] & 0x0fU) == 0xA);        /* pong */
        CHECK((frame[1] & 0x7fU) == 2);
    }

    /* A close frame surfaces as a close. */
    CHECK(send_server_frame(peer.peer_fd, 0x8, 1, "\003\350", 2) == 0);
    CHECK(le_ws_read_text(&ws, message, sizeof(message), 500) == 2);

    /* A masked server frame is a protocol violation and is refused. */
    {
        unsigned char bad[6] = {0x81, 0x82, 1, 2, 3, 4};

        CHECK(write(peer.peer_fd, bad, sizeof(bad)) == (ssize_t)sizeof(bad));
        CHECK(le_ws_read_text(&ws, message, sizeof(message), 500) == -1);
    }
    /* A fragmented frame is not supported and is refused rather than
       half-assembled. */
    {
        struct le_ws fresh;
        struct peer other;
        int other_pair[2];

        CHECK(setup(&fresh, &stream, &other, other_pair) == 0);
        CHECK(send_server_frame(other_pair[1], 0x1, 0, "part", 4) == 0);
        CHECK(le_ws_read_text(&fresh, message, sizeof(message), 500) == -1);
        close(other_pair[0]);
        close(other_pair[1]);
        le_ws_close(&fresh);
    }

    close(pair[0]);
    close(pair[1]);
    le_ws_close(&ws);
    return 0;
}

static int test_oversized_frame_is_refused(void)
{
    struct le_ws ws;
    struct le_ws_stream stream;
    struct peer peer;
    int pair[2];
    char message[256];
    unsigned char header[10];

    CHECK(setup(&ws, &stream, &peer, pair) == 0);
    /* Declare more than the receive buffer holds: the 64-bit length form with
       a value of 4 GiB - 1. */
    header[0] = 0x81;
    header[1] = 127;
    memset(header + 2, 0, 4);
    memset(header + 6, 0xff, 4);
    CHECK(write(peer.peer_fd, header, sizeof(header)) == (ssize_t)sizeof(header));
    CHECK(le_ws_read_text(&ws, message, sizeof(message), 500) == -1);
    /* And a text frame larger than the caller's destination. */
    CHECK(send_server_frame(peer.peer_fd, 0x1, 1, "0123456789", 10) == 0);
    CHECK(le_ws_read_text(&ws, message, 4, 500) == -1);

    close(pair[0]);
    close(pair[1]);
    le_ws_close(&ws);
    return 0;
}

static int test_timeout_reports_nothing(void)
{
    struct le_ws ws;
    struct le_ws_stream stream;
    struct peer peer;
    int pair[2];
    char message[64];

    CHECK(setup(&ws, &stream, &peer, pair) == 0);
    CHECK(le_ws_read_text(&ws, message, sizeof(message), 60) == 0);
    close(pair[0]);
    close(pair[1]);
    le_ws_close(&ws);
    return 0;
}

static int test_stream_error_is_not_a_clean_close(void)
{
    struct le_ws ws;
    struct le_ws_stream stream;
    char message[64];

    memset(&ws, 0, sizeof(ws));
    memset(&stream, 0, sizeof(stream));
    stream.recv = failing_recv;
    ws.stream = &stream;
    ws.connected = 1;
    CHECK(le_ws_read_text(&ws, message, sizeof(message), 60) == -1);
    return 0;
}

static int test_fragmented_tcp_delivery_and_zero_poll(void)
{
    struct le_ws ws;
    struct le_ws_stream stream;
    struct peer peer;
    int pair[2];
    unsigned char frame[134];
    char output[256];
    struct timespec before,after;
    size_t i;
    CHECK(setup(&ws,&stream,&peer,pair)==0);
    frame[0]=0x81;frame[1]=126;frame[2]=0;frame[3]=130;
    memset(frame+4,'x',130);
    for(i=0;i<sizeof(frame);++i) {
        CHECK(write(pair[1],frame+i,1)==1);
        CHECK(le_ws_read_text(&ws,output,sizeof(output),0)==(i+1==sizeof(frame)?1:0));
    }
    CHECK(strlen(output)==130 && ws.frames_in==1);
    clock_gettime(CLOCK_MONOTONIC,&before);
    CHECK(le_ws_read_text(&ws,output,sizeof(output),0)==0);
    clock_gettime(CLOCK_MONOTONIC,&after);
    CHECK((after.tv_sec-before.tv_sec)*1000000000LL+after.tv_nsec-before.tv_nsec < 30000000LL);
    close(pair[0]);close(pair[1]);le_ws_close(&ws);return 0;
}
static long blocked_send(void *context, const void *buffer, size_t size)
{
    (void)context;(void)buffer;(void)size;return -2;
}
static int test_backpressure_retains_masked_frame(void)
{
    struct le_ws ws;
    struct le_ws_stream stream;
    struct peer peer;
    int pair[2];
    unsigned char received[32];
    const char *text="hello";
    size_t i;
    CHECK(setup(&ws,&stream,&peer,pair)==0);
    stream.send=blocked_send;
    CHECK(le_ws_send_text(&ws,text,5)==0);
    CHECK(ws.tx_chunk_used==11 && ws.tx_chunk_sent==0);
    CHECK(le_ws_pump(&ws)==0 && ws.tx_chunk_sent==0);
    stream.send=peer_send;
    CHECK(le_ws_pump(&ws)==0);
    CHECK(drain(pair[1],received,11)==0);
    CHECK(received[0]==0x81 && received[1]==0x85);
    for(i=0;i<5;++i) CHECK((received[6+i]^received[2+i%4])==(unsigned char)text[i]);
    close(pair[0]);close(pair[1]);le_ws_close(&ws);return 0;
}

int main(void)
{
    int failures = 0;

    /* The suite runs on a build machine with no operator watching. */
    alarm(60);

    failures += test_fragmented_tcp_delivery_and_zero_poll() != 0;
    failures += test_backpressure_retains_masked_frame() != 0;
    failures += test_base64() != 0;
    failures += test_client_frames_are_masked() != 0;
    failures += test_server_frames_are_read_and_validated() != 0;
    failures += test_oversized_frame_is_refused() != 0;
    failures += test_timeout_reports_nothing() != 0;
    failures += test_stream_error_is_not_a_clean_close() != 0;
    if (failures) {
        fprintf(stderr, "ws client: FAILED\n");
        return 1;
    }
    printf("ws client: ok\n");
    return 0;
}
