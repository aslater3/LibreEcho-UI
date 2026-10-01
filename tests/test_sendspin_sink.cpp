// SPDX-License-Identifier: MIT
//
// Sendspin native bridge — engine-sink client unit tests (C++20).
//
// These drive the C++20 EngineSink client against the *real* compiled Platform
// LE_AUDIO_SINK/1 server (audio_sink.c) and, for adversarial replies the real
// engine would never emit, against a minimal scripted raw peer. They pin the
// Task 5 bridge contract:
//
//   * on_audio_write returns ACKNOWLEDGED BYTES (whole frames), never the bytes
//     merely queued to the kernel and never un-acked frames;
//   * bounded partial acceptance (a full engine queue) returns a whole-frame
//     prefix, not an error and not a lie;
//   * a stalled engine yields zero accepted bytes, never a false success;
//   * malformed / truncated / wrong-magic / wrong-version / not-ready replies
//     and non-OK or wrong-epoch / wrong-generation / wrong-sequence CREDITs are
//     never counted as acceptance;
//   * the acknowledged PCM is byte-exact and ordered.

#include "engine_sink.h"
#include "sendspin_engine_fixture.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using libreecho::sendspin::EngineSink;
using libreecho::sendspin::EngineSinkTiming;
using libreecho::sendspin::SinkResult;
using namespace libreecho::sendspin::test;

namespace {

// --- crafted replies (ADR: only the scripted peer uses these) ---------------

void build_open_ack(uint8_t* out, uint32_t epoch, uint32_t generation,
                    uint32_t status = LE_AUDIO_SINK_OK,
                    uint32_t readiness = LE_AUDIO_SINK_OPEN_READY,
                    uint32_t rate = LE_AUDIO_SINK_OUTPUT_RATE,
                    uint16_t channels = LE_AUDIO_SINK_OUTPUT_CHANNELS,
                    uint16_t format = LE_AUDIO_SINK_FORMAT_S16_LE,
                    uint32_t magic = LE_AUDIO_SINK_MAGIC,
                    uint8_t version = LE_AUDIO_SINK_PROTOCOL_VERSION) {
    le_audio_sink_encode_header(out, LE_AUDIO_SINK_TYPE_OPEN_ACK,
                                LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES);
    le_audio_sink_put_u32(out, magic);  // overwrite magic for negative cases
    out[4] = version;
    uint8_t* p = out + LE_AUDIO_SINK_HEADER_BYTES;
    le_audio_sink_put_u32(p + 0, status);
    le_audio_sink_put_u32(p + 4, epoch);
    le_audio_sink_put_u32(p + 8, generation);
    le_audio_sink_put_u32(p + 12, rate);
    le_audio_sink_put_u16(p + 16, channels);
    le_audio_sink_put_u16(p + 18, format);
    le_audio_sink_put_u32(p + 20, LE_AUDIO_SINK_PERIOD_FRAMES);
    le_audio_sink_put_u32(p + 24, LE_AUDIO_SINK_MAX_CAPACITY_FRAMES);
    le_audio_sink_put_u32(p + 28, readiness);
    le_audio_sink_put_u32(p + 32, 0);
}

void build_credit(uint8_t* out, uint32_t epoch, uint32_t generation,
                  uint32_t sequence, uint32_t status, uint64_t accepted,
                  uint32_t remaining) {
    le_audio_sink_encode_header(out, LE_AUDIO_SINK_TYPE_CREDIT,
                                LE_AUDIO_SINK_CREDIT_PAYLOAD_BYTES);
    uint8_t* p = out + LE_AUDIO_SINK_HEADER_BYTES;
    le_audio_sink_put_u32(p + 0, status);
    le_audio_sink_put_u32(p + 4, epoch);
    le_audio_sink_put_u32(p + 8, generation);
    le_audio_sink_put_u32(p + 12, sequence);
    le_audio_sink_put_u64(p + 16, accepted);
    le_audio_sink_put_u32(p + 24, remaining);
    le_audio_sink_put_u32(p + 28, 0);
}

void build_error(uint8_t* out, uint32_t status, uint32_t epoch,
                 uint32_t generation, uint32_t detail) {
    le_audio_sink_encode_header(out, LE_AUDIO_SINK_TYPE_ERROR,
                                LE_AUDIO_SINK_ERROR_PAYLOAD_BYTES);
    uint8_t* p = out + LE_AUDIO_SINK_HEADER_BYTES;
    le_audio_sink_put_u32(p + 0, status);
    le_audio_sink_put_u32(p + 4, epoch);
    le_audio_sink_put_u32(p + 8, generation);
    le_audio_sink_put_u32(p + 12, detail);
    le_audio_sink_put_u32(p + 16, 0);
}

// --- tests ------------------------------------------------------------------

void test_connect_and_open_round_trip() {
    std::string path = unique_socket_path("sink");
    EngineServer server;
    CHECK(server.start(path), "real engine server start");

    EngineSink client;
    CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect ok");
    CHECK(client.connected(), "client reports connected");
    CHECK(client.open(1, 2000) == SinkResult::Ok, "open ok");
    CHECK(client.epoch() == server.epoch(), "client adopted the engine epoch");
    CHECK(client.epoch() != 0u, "epoch non-zero");
    CHECK(client.generation() == 1u, "generation adopted");
    CHECK(client.accepted_frames() == 0u, "no frames accepted before DATA");

    client.disconnect();
    CHECK(!client.connected(), "disconnect closes the socket");
    server.stop();
}

void test_acknowledged_bytes_are_exact_and_ordered() {
    std::string path = unique_socket_path("sink");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSink client;
    CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
    CHECK(client.open(1, 2000) == SinkResult::Ok, "open");

    const size_t frames = 4096;  // two full periods, within engine capacity
    std::vector<int16_t> pcm(frames * 2);
    fill_pattern(pcm.data(), frames);

    size_t accepted =
        client.on_audio_write(reinterpret_cast<uint8_t*>(pcm.data()),
                              frames * kBytesPerFrame, 3000);
    CHECK(accepted == frames * kBytesPerFrame, "all offered bytes acknowledged");
    CHECK(accepted % kBytesPerFrame == 0, "accepted bytes are whole frames");
    CHECK(client.accepted_frames() == frames, "cumulative accepted frames exact");
    CHECK(client.sequence() == 2u,
          "sequence advanced once per accepted DATA (two bounded datagrams)");

    // The real engine holds exactly the bytes the client acknowledged, ordered.
    std::vector<int16_t> out(frames * 2, 0);
    uint64_t first = 999;
    size_t got = le_audio_sink_render(server.sink(), out.data(), frames, &first);
    CHECK(got == frames, "engine rendered the full acknowledged block");
    CHECK(first == 0u, "first rendered frame index is zero");
    CHECK(std::memcmp(out.data(), pcm.data(), frames * kBytesPerFrame) == 0,
          "acknowledged PCM is byte-exact and ordered on the engine");

    client.disconnect();
    server.stop();
}

void test_bounded_partial_acceptance_is_whole_frames() {
    std::string path = unique_socket_path("sink");
    EngineServer server;
    // Capacity of exactly one period: the second DATA of this offer is refused.
    CHECK(server.start(path, LE_AUDIO_SINK_PERIOD_FRAMES), "server start");

    EngineSink client;
    CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
    CHECK(client.open(1, 2000) == SinkResult::Ok, "open");

    const size_t frames = 3 * LE_AUDIO_SINK_PERIOD_FRAMES;  // 6144
    std::vector<int16_t> pcm(frames * 2);
    fill_pattern(pcm.data(), frames);

    size_t accepted =
        client.on_audio_write(reinterpret_cast<uint8_t*>(pcm.data()),
                              frames * kBytesPerFrame, 3000);
    // Exactly the first bounded datagram is acknowledged, whole frames only.
    CHECK(accepted == LE_AUDIO_SINK_PERIOD_FRAMES * kBytesPerFrame,
          "bounded acceptance returned a whole-frame prefix");
    CHECK(accepted % kBytesPerFrame == 0, "partial return is frame-aligned");
    CHECK(accepted < frames * kBytesPerFrame, "over-capacity tail was not claimed");
    CHECK(client.accepted_frames() == LE_AUDIO_SINK_PERIOD_FRAMES,
          "cumulative accepted matches the bounded prefix");
    CHECK(client.last_error() == LE_AUDIO_SINK_ERR_CAPACITY,
          "capacity rejection recorded, not silently dropped");

    client.disconnect();
    server.stop();
}

void test_stalled_engine_returns_zero_not_queued_bytes() {
    std::string path = unique_socket_path("sink");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSink client;
    CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
    CHECK(client.open(1, 2000) == SinkResult::Ok, "open");

    // Stop servicing: the send will succeed into the kernel buffer, but no
    // CREDIT can ever arrive. A successful send must not be mistaken for
    // application acceptance.
    server.pause();

    std::vector<int16_t> pcm(LE_AUDIO_SINK_PERIOD_FRAMES * 2);
    fill_pattern(pcm.data(), LE_AUDIO_SINK_PERIOD_FRAMES);
    size_t accepted =
        client.on_audio_write(reinterpret_cast<uint8_t*>(pcm.data()),
                              LE_AUDIO_SINK_PERIOD_FRAMES * kBytesPerFrame, 250);
    CHECK(accepted == 0u, "stalled engine yielded zero accepted bytes");
    CHECK(client.accepted_frames() == 0u, "no frames credited without a CREDIT");

    server.resume();
    client.disconnect();
    server.stop();
}

void test_malformed_open_ack_is_rejected() {
    // Truncated header.
    {
        std::string path = unique_socket_path("peer");
        RawPeer peer;
        CHECK(peer.start(path, [](int fd) {
                  uint8_t buf[2048];
                  (void)raw_recv(fd, buf, sizeof(buf));
                  uint8_t junk[10] = {0};
                  raw_send(fd, junk, sizeof(junk));
              }),
              "peer start");
        EngineSink client;
        CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
        CHECK(client.open(1, 500) == SinkResult::Malformed,
              "truncated OPEN_ACK rejected as malformed");
    }
    // Wrong magic.
    {
        std::string path = unique_socket_path("peer");
        RawPeer peer;
        CHECK(peer.start(path, [](int fd) {
                  uint8_t buf[2048];
                  (void)raw_recv(fd, buf, sizeof(buf));
                  uint8_t out[LE_AUDIO_SINK_HEADER_BYTES +
                              LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
                  build_open_ack(out, 0x1234, 1, LE_AUDIO_SINK_OK,
                                 LE_AUDIO_SINK_OPEN_READY,
                                 LE_AUDIO_SINK_OUTPUT_RATE,
                                 LE_AUDIO_SINK_OUTPUT_CHANNELS,
                                 LE_AUDIO_SINK_FORMAT_S16_LE, 0u /*magic*/);
                  raw_send(fd, out, sizeof(out));
              }),
              "peer start");
        EngineSink client;
        CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
        CHECK(client.open(1, 500) == SinkResult::Malformed,
              "wrong-magic OPEN_ACK rejected");
    }
    // Wrong protocol version.
    {
        std::string path = unique_socket_path("peer");
        RawPeer peer;
        CHECK(peer.start(path, [](int fd) {
                  uint8_t buf[2048];
                  (void)raw_recv(fd, buf, sizeof(buf));
                  uint8_t out[LE_AUDIO_SINK_HEADER_BYTES +
                              LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
                  build_open_ack(out, 0x1234, 1, LE_AUDIO_SINK_OK,
                                 LE_AUDIO_SINK_OPEN_READY,
                                 LE_AUDIO_SINK_OUTPUT_RATE,
                                 LE_AUDIO_SINK_OUTPUT_CHANNELS,
                                 LE_AUDIO_SINK_FORMAT_S16_LE,
                                 LE_AUDIO_SINK_MAGIC, 9u /*version*/);
                  raw_send(fd, out, sizeof(out));
              }),
              "peer start");
        EngineSink client;
        CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
        CHECK(client.open(1, 500) == SinkResult::Malformed,
              "wrong-version OPEN_ACK rejected");
    }
    // Non-OK status.
    {
        std::string path = unique_socket_path("peer");
        RawPeer peer;
        CHECK(peer.start(path, [](int fd) {
                  uint8_t buf[2048];
                  (void)raw_recv(fd, buf, sizeof(buf));
                  uint8_t out[LE_AUDIO_SINK_HEADER_BYTES +
                              LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
                  build_open_ack(out, 0x1234, 1, LE_AUDIO_SINK_ERR_GEOMETRY);
                  raw_send(fd, out, sizeof(out));
              }),
              "peer start");
        EngineSink client;
        CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
        CHECK(client.open(1, 500) == SinkResult::Rejected,
              "non-OK OPEN_ACK status rejected");
        CHECK(client.last_error() == LE_AUDIO_SINK_ERR_GEOMETRY,
              "engine status recorded");
    }
    // Not-ready readiness bitmap.
    {
        std::string path = unique_socket_path("peer");
        RawPeer peer;
        CHECK(peer.start(path, [](int fd) {
                  uint8_t buf[2048];
                  (void)raw_recv(fd, buf, sizeof(buf));
                  uint8_t out[LE_AUDIO_SINK_HEADER_BYTES +
                              LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
                  build_open_ack(out, 0x1234, 1, LE_AUDIO_SINK_OK, 0u /*not ready*/);
                  raw_send(fd, out, sizeof(out));
              }),
              "peer start");
        EngineSink client;
        CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
        CHECK(client.open(1, 500) == SinkResult::Malformed,
              "not-ready OPEN_ACK rejected");
    }
}

void test_stale_or_bad_credit_never_counts() {
    // Each case: the peer acknowledges OPEN normally, then this crafts the CREDIT
    // (or ERROR) for the first DATA. None may count as acceptance.
    struct Case {
        const char* name;
        bool send_error;
        uint32_t epoch_delta;
        uint32_t generation;
        uint32_t sequence;
        uint32_t status;
    };
    const Case cases[] = {
        {"wrong epoch", false, 1u, 1u, 0u, LE_AUDIO_SINK_OK},
        {"wrong generation", false, 0u, 2u, 0u, LE_AUDIO_SINK_OK},
        {"wrong sequence", false, 0u, 1u, 5u, LE_AUDIO_SINK_OK},
        {"non-ok status", false, 0u, 1u, 0u, LE_AUDIO_SINK_ERR_CAPACITY},
    };

    for (const Case& c : cases) {
        std::string path = unique_socket_path("peer");
        RawPeer peer;
        const uint32_t peer_epoch = 0x00A1B2C3u;
        Case captured = c;
        CHECK(peer.start(path, [captured, peer_epoch](int fd) {
                  uint8_t buf[4096];
                  ssize_t n = raw_recv(fd, buf, sizeof(buf));
                  if (n <= 0) return;  // OPEN
                  uint8_t ack[LE_AUDIO_SINK_HEADER_BYTES +
                              LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
                  build_open_ack(ack, peer_epoch, 1);
                  raw_send(fd, ack, sizeof(ack));
                  n = raw_recv(fd, buf, sizeof(buf));
                  if (n <= 0) return;  // DATA
                  uint8_t reply[LE_AUDIO_SINK_HEADER_BYTES + 32];
                  if (captured.send_error) {
                      build_error(reply, LE_AUDIO_SINK_ERR_STALE_GENERATION,
                                  peer_epoch, 1, LE_AUDIO_SINK_TYPE_DATA);
                      raw_send(fd, reply,
                               LE_AUDIO_SINK_HEADER_BYTES +
                                   LE_AUDIO_SINK_ERROR_PAYLOAD_BYTES);
                  } else {
                      build_credit(reply, peer_epoch + captured.epoch_delta,
                                   captured.generation, captured.sequence,
                                   captured.status, 2048u, 0u);
                      raw_send(fd, reply, sizeof(reply));
                  }
              }),
              "peer start");

        EngineSink client;
        CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
        CHECK(client.open(1, 1000) == SinkResult::Ok, "open");
        CHECK(client.epoch() == peer_epoch, "adopted crafted epoch");

        std::vector<int16_t> pcm(2048 * 2);
        fill_pattern(pcm.data(), 2048);
        size_t accepted =
            client.on_audio_write(reinterpret_cast<uint8_t*>(pcm.data()),
                                  2048 * kBytesPerFrame, 500);
        char label[128];
        std::snprintf(label, sizeof(label), "%s: bad CREDIT not counted", c.name);
        CHECK(accepted == 0u, label);
        CHECK(client.accepted_frames() == 0u, "no frame credited for bad CREDIT");
        peer.stop();
    }

    // Explicit ERROR reply for DATA is surfaced and never counted.
    {
        std::string path = unique_socket_path("peer");
        RawPeer peer;
        CHECK(peer.start(path, [](int fd) {
                  uint8_t buf[4096];
                  if (raw_recv(fd, buf, sizeof(buf)) <= 0) return;
                  uint8_t ack[LE_AUDIO_SINK_HEADER_BYTES +
                              LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
                  build_open_ack(ack, 0x55, 1);
                  raw_send(fd, ack, sizeof(ack));
                  if (raw_recv(fd, buf, sizeof(buf)) <= 0) return;
                  uint8_t err[LE_AUDIO_SINK_HEADER_BYTES +
                              LE_AUDIO_SINK_ERROR_PAYLOAD_BYTES];
                  build_error(err, LE_AUDIO_SINK_ERR_FINISHED, 0x55, 1,
                              LE_AUDIO_SINK_TYPE_DATA);
                  raw_send(fd, err, sizeof(err));
              }),
              "peer start");
        EngineSink client;
        CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
        CHECK(client.open(1, 1000) == SinkResult::Ok, "open");
        std::vector<int16_t> pcm(2048 * 2);
        fill_pattern(pcm.data(), 2048);
        size_t accepted =
            client.on_audio_write(reinterpret_cast<uint8_t*>(pcm.data()),
                                  2048 * kBytesPerFrame, 500);
        CHECK(accepted == 0u, "ERROR reply not counted as acceptance");
        CHECK(client.last_error() == LE_AUDIO_SINK_ERR_FINISHED,
              "ERROR status surfaced");
    }
}

}  // namespace

int main() {
    std::fprintf(stderr,
                 "test_sendspin_sink: engine-sink client vs real C99 engine\n");
    test_connect_and_open_round_trip();
    test_acknowledged_bytes_are_exact_and_ordered();
    test_bounded_partial_acceptance_is_whole_frames();
    test_stalled_engine_returns_zero_not_queued_bytes();
    test_malformed_open_ack_is_rejected();
    test_stale_or_bad_credit_never_counts();

    if (g_failures != 0) {
        std::fprintf(stderr, "test_sendspin_sink: %d check(s) FAILED\n",
                     g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_sendspin_sink: all checks passed\n");
    return 0;
}
