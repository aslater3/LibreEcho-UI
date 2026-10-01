// SPDX-License-Identifier: MIT
//
// Sendspin native bridge — reply-validation regression tests (C++20, RED-first).
//
// These pin the three ack-validation gaps the independent SPEC/QUALITY reviews
// flagged (F1/F2/F3) plus the deliberate frame-alignment rule, all of which are
// only reachable against a *scripted raw peer*: the real uid-validated engine
// emits every reply through require_active, so it would never send a mismatched
// epoch. The peer here drives the client's own reply validation with bytes the
// real engine cannot produce.
//
//   F1  FINISH_ACK epoch (pl+4) must match the live epoch before the client
//       disarms; a mismatch is StaleEpoch and mutates nothing.
//   F2  RESET_ACK old_epoch (pl+4) must match the live epoch before the client
//       fences; a mismatch is StaleEpoch and mutates nothing.
//   F3  CREDIT accepted_frames (pl+16) must equal the acknowledged cumulative
//       cursor after this chunk; an impossible value is rejected without
//       advancing the cursor.
//   ALIGN  a non-frame-aligned offered length is rejected deliberately instead
//       of silently dropping the trailing partial frame.
//
// Positive controls keep the checks from being over-strict: a well-formed
// FINISH_ACK / RESET_ACK / CREDIT must still be honoured.

#include "engine_sink.h"
#include "sendspin_engine_fixture.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using libreecho::sendspin::EngineSink;
using libreecho::sendspin::SinkResult;
using namespace libreecho::sendspin::test;

namespace {

constexpr uint32_t kEpoch = 0x11112222u;

void build_open_ack(uint8_t* out, uint32_t epoch, uint32_t generation) {
    le_audio_sink_encode_header(out, LE_AUDIO_SINK_TYPE_OPEN_ACK,
                                LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES);
    uint8_t* p = out + LE_AUDIO_SINK_HEADER_BYTES;
    le_audio_sink_put_u32(p + 0, LE_AUDIO_SINK_OK);
    le_audio_sink_put_u32(p + 4, epoch);
    le_audio_sink_put_u32(p + 8, generation);
    le_audio_sink_put_u32(p + 12, LE_AUDIO_SINK_OUTPUT_RATE);
    le_audio_sink_put_u16(p + 16, LE_AUDIO_SINK_OUTPUT_CHANNELS);
    le_audio_sink_put_u16(p + 18, LE_AUDIO_SINK_FORMAT_S16_LE);
    le_audio_sink_put_u32(p + 20, LE_AUDIO_SINK_PERIOD_FRAMES);
    le_audio_sink_put_u32(p + 24, LE_AUDIO_SINK_MAX_CAPACITY_FRAMES);
    le_audio_sink_put_u32(p + 28, LE_AUDIO_SINK_OPEN_READY);
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

void build_finish_ack(uint8_t* out, uint32_t status, uint32_t epoch,
                      uint32_t generation, uint64_t total, uint32_t completed) {
    le_audio_sink_encode_header(out, LE_AUDIO_SINK_TYPE_FINISH_ACK,
                                LE_AUDIO_SINK_FINISH_ACK_PAYLOAD_BYTES);
    uint8_t* p = out + LE_AUDIO_SINK_HEADER_BYTES;
    le_audio_sink_put_u32(p + 0, status);
    le_audio_sink_put_u32(p + 4, epoch);
    le_audio_sink_put_u32(p + 8, generation);
    le_audio_sink_put_u64(p + 12, total);
    le_audio_sink_put_u32(p + 20, completed);
    le_audio_sink_put_u32(p + 24, 0);
}

void build_reset_ack(uint8_t* out, uint32_t status, uint32_t old_epoch,
                     uint32_t old_generation) {
    le_audio_sink_encode_header(out, LE_AUDIO_SINK_TYPE_RESET_ACK,
                                LE_AUDIO_SINK_RESET_ACK_PAYLOAD_BYTES);
    uint8_t* p = out + LE_AUDIO_SINK_HEADER_BYTES;
    le_audio_sink_put_u32(p + 0, status);
    le_audio_sink_put_u32(p + 4, old_epoch);
    le_audio_sink_put_u32(p + 8, old_generation);
    le_audio_sink_put_u32(p + 12, 0);
    le_audio_sink_put_u64(p + 16, 0);
    le_audio_sink_put_u32(p + 24, 0);
    le_audio_sink_put_u32(p + 28, 0);
}

size_t drive_open(EngineSink& client, const std::string& path, uint32_t gen) {
    if (client.connect(path, 2000) != SinkResult::Ok) return 0;
    return client.open(gen, 2000) == SinkResult::Ok ? 1u : 0u;
}

size_t write_frames(EngineSink& client, size_t frames, uint32_t timeout_ms) {
    std::vector<int16_t> pcm(frames * 2);
    fill_pattern(pcm.data(), frames);
    return client.on_audio_write(reinterpret_cast<uint8_t*>(pcm.data()),
                                 frames * kBytesPerFrame, timeout_ms);
}

// ---------------------------------------------------------------------------
// F1 — FINISH_ACK epoch is validated before disarming.
// ---------------------------------------------------------------------------
void test_finish_ack_epoch() {
    // Negative: wrong epoch -> StaleEpoch, still armed, no mutation.
    {
        std::string path = unique_socket_path("ack");
        const uint32_t gen = 7;
        RawPeer peer;
        CHECK(peer.start(path, [&](int fd) {
                  uint8_t buf[4096];
                  raw_recv(fd, buf, sizeof(buf));  // OPEN
                  uint8_t ack[LE_AUDIO_SINK_HEADER_BYTES +
                              LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
                  build_open_ack(ack, kEpoch, gen);
                  raw_send(fd, ack, sizeof(ack));
                  raw_recv(fd, buf, sizeof(buf));  // DATA
                  uint8_t cr[LE_AUDIO_SINK_HEADER_BYTES +
                             LE_AUDIO_SINK_CREDIT_PAYLOAD_BYTES];
                  build_credit(cr, kEpoch, gen, 0, LE_AUDIO_SINK_OK, 256u, 0);
                  raw_send(fd, cr, sizeof(cr));
                  raw_recv(fd, buf, sizeof(buf));  // FINISH
                  uint8_t fa[LE_AUDIO_SINK_HEADER_BYTES +
                             LE_AUDIO_SINK_FINISH_ACK_PAYLOAD_BYTES];
                  build_finish_ack(fa, LE_AUDIO_SINK_OK, 0xDEADDEADu, gen, 256u,
                                   0);
                  raw_send(fd, fa, sizeof(fa));
                  raw_recv(fd, buf, sizeof(buf));  // drain until client closes
              }),
              "raw peer start");

        EngineSink client;
        CHECK(drive_open(client, path, gen) == 1u, "open");
        CHECK(write_frames(client, 256, 1000) == 256 * kBytesPerFrame,
              "credit accepted");
        bool completed = true;
        CHECK(client.finish(256, 1000, &completed) == SinkResult::StaleEpoch,
              "wrong-epoch FINISH_ACK is StaleEpoch");
        CHECK(client.open_generation(),
              "stale FINISH_ACK must not disarm the live generation");
        CHECK(client.accepted_frames() == 256u,
              "stale FINISH_ACK must not mutate accepted progress");
        client.disconnect();
        peer.stop();
    }
    // Positive control: the right epoch is still honoured and disarms.
    {
        std::string path = unique_socket_path("ack");
        const uint32_t gen = 3;
        RawPeer peer;
        CHECK(peer.start(path, [&](int fd) {
                  uint8_t buf[4096];
                  raw_recv(fd, buf, sizeof(buf));  // OPEN
                  uint8_t ack[LE_AUDIO_SINK_HEADER_BYTES +
                              LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
                  build_open_ack(ack, kEpoch, gen);
                  raw_send(fd, ack, sizeof(ack));
                  raw_recv(fd, buf, sizeof(buf));  // FINISH
                  uint8_t fa[LE_AUDIO_SINK_HEADER_BYTES +
                             LE_AUDIO_SINK_FINISH_ACK_PAYLOAD_BYTES];
                  build_finish_ack(fa, LE_AUDIO_SINK_OK, kEpoch, gen, 0u, 0);
                  raw_send(fd, fa, sizeof(fa));
                  raw_recv(fd, buf, sizeof(buf));
              }),
              "raw peer start");
        EngineSink client;
        CHECK(drive_open(client, path, gen) == 1u, "open");
        bool completed = true;
        CHECK(client.finish(0, 1000, &completed) == SinkResult::Ok,
              "correct-epoch FINISH_ACK still accepted");
        CHECK(!client.open_generation(), "a valid FINISH still disarms");
        client.disconnect();
        peer.stop();
    }
}

// ---------------------------------------------------------------------------
// F2 — RESET_ACK old_epoch is validated before fencing.
// ---------------------------------------------------------------------------
void test_reset_ack_old_epoch() {
    // Negative: wrong old_epoch -> StaleEpoch, still armed, queue untouched.
    {
        std::string path = unique_socket_path("ack");
        const uint32_t gen = 9;
        RawPeer peer;
        CHECK(peer.start(path, [&](int fd) {
                  uint8_t buf[4096];
                  raw_recv(fd, buf, sizeof(buf));  // OPEN
                  uint8_t ack[LE_AUDIO_SINK_HEADER_BYTES +
                              LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
                  build_open_ack(ack, kEpoch, gen);
                  raw_send(fd, ack, sizeof(ack));
                  raw_recv(fd, buf, sizeof(buf));  // DATA
                  uint8_t cr[LE_AUDIO_SINK_HEADER_BYTES +
                             LE_AUDIO_SINK_CREDIT_PAYLOAD_BYTES];
                  build_credit(cr, kEpoch, gen, 0, LE_AUDIO_SINK_OK, 128u, 0);
                  raw_send(fd, cr, sizeof(cr));
                  raw_recv(fd, buf, sizeof(buf));  // CANCEL
                  uint8_t ra[LE_AUDIO_SINK_HEADER_BYTES +
                             LE_AUDIO_SINK_RESET_ACK_PAYLOAD_BYTES];
                  build_reset_ack(ra, LE_AUDIO_SINK_OK, 0xBAD0BAD0u, gen);
                  raw_send(fd, ra, sizeof(ra));
                  raw_recv(fd, buf, sizeof(buf));
              }),
              "raw peer start");
        EngineSink client;
        CHECK(drive_open(client, path, gen) == 1u, "open");
        CHECK(write_frames(client, 128, 1000) == 128 * kBytesPerFrame,
              "credit accepted");
        CHECK(client.cancel(1000) == SinkResult::StaleEpoch,
              "wrong-old-epoch RESET_ACK is StaleEpoch");
        CHECK(client.open_generation(),
              "stale RESET_ACK must not fence the live generation");
        CHECK(client.accepted_frames() == 128u,
              "stale RESET_ACK must not discard the source cursor");
        client.disconnect();
        peer.stop();
    }
    // Positive control: the right old_epoch still fences.
    {
        std::string path = unique_socket_path("ack");
        const uint32_t gen = 4;
        RawPeer peer;
        CHECK(peer.start(path, [&](int fd) {
                  uint8_t buf[4096];
                  raw_recv(fd, buf, sizeof(buf));  // OPEN
                  uint8_t ack[LE_AUDIO_SINK_HEADER_BYTES +
                              LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
                  build_open_ack(ack, kEpoch, gen);
                  raw_send(fd, ack, sizeof(ack));
                  raw_recv(fd, buf, sizeof(buf));  // CANCEL
                  uint8_t ra[LE_AUDIO_SINK_HEADER_BYTES +
                             LE_AUDIO_SINK_RESET_ACK_PAYLOAD_BYTES];
                  build_reset_ack(ra, LE_AUDIO_SINK_OK, kEpoch, gen);
                  raw_send(fd, ra, sizeof(ra));
                  raw_recv(fd, buf, sizeof(buf));
              }),
              "raw peer start");
        EngineSink client;
        CHECK(drive_open(client, path, gen) == 1u, "open");
        CHECK(client.cancel(1000) == SinkResult::Ok,
              "correct-old-epoch RESET_ACK still fences");
        CHECK(!client.open_generation(), "a valid RESET still fences");
        client.disconnect();
        peer.stop();
    }
}

// ---------------------------------------------------------------------------
// F3 — CREDIT accepted_frames must be consistent with the acked prefix.
// ---------------------------------------------------------------------------
void test_credit_accepted_frames() {
    // Negative: impossible accepted_frames -> not counted, cursor untouched.
    {
        std::string path = unique_socket_path("ack");
        const uint32_t gen = 2;
        RawPeer peer;
        CHECK(peer.start(path, [&](int fd) {
                  uint8_t buf[4096];
                  raw_recv(fd, buf, sizeof(buf));  // OPEN
                  uint8_t ack[LE_AUDIO_SINK_HEADER_BYTES +
                              LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
                  build_open_ack(ack, kEpoch, gen);
                  raw_send(fd, ack, sizeof(ack));
                  raw_recv(fd, buf, sizeof(buf));  // DATA
                  uint8_t cr[LE_AUDIO_SINK_HEADER_BYTES +
                             LE_AUDIO_SINK_CREDIT_PAYLOAD_BYTES];
                  build_credit(cr, kEpoch, gen, 0, LE_AUDIO_SINK_OK, 999999u, 0);
                  raw_send(fd, cr, sizeof(cr));
                  raw_recv(fd, buf, sizeof(buf));
              }),
              "raw peer start");
        EngineSink client;
        CHECK(drive_open(client, path, gen) == 1u, "open");
        CHECK(write_frames(client, 64, 1000) == 0u,
              "a CREDIT with an impossible accepted_frames is not counted");
        CHECK(client.accepted_frames() == 0u,
              "the acknowledged cursor did not advance on an impossible CREDIT");
        client.disconnect();
        peer.stop();
    }
    // Positive control: consistent accepted_frames is honoured exactly.
    {
        std::string path = unique_socket_path("ack");
        const uint32_t gen = 2;
        RawPeer peer;
        CHECK(peer.start(path, [&](int fd) {
                  uint8_t buf[4096];
                  raw_recv(fd, buf, sizeof(buf));  // OPEN
                  uint8_t ack[LE_AUDIO_SINK_HEADER_BYTES +
                              LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
                  build_open_ack(ack, kEpoch, gen);
                  raw_send(fd, ack, sizeof(ack));
                  raw_recv(fd, buf, sizeof(buf));  // DATA
                  uint8_t cr[LE_AUDIO_SINK_HEADER_BYTES +
                             LE_AUDIO_SINK_CREDIT_PAYLOAD_BYTES];
                  build_credit(cr, kEpoch, gen, 0, LE_AUDIO_SINK_OK, 64u, 0);
                  raw_send(fd, cr, sizeof(cr));
                  raw_recv(fd, buf, sizeof(buf));
              }),
              "raw peer start");
        EngineSink client;
        CHECK(drive_open(client, path, gen) == 1u, "open");
        CHECK(write_frames(client, 64, 1000) == 64 * kBytesPerFrame,
              "a consistent CREDIT is accepted exactly");
        CHECK(client.accepted_frames() == 64u, "cursor advanced exactly once");
        client.disconnect();
        peer.stop();
    }
}

// ---------------------------------------------------------------------------
// ALIGN — a non-frame-aligned offered length is rejected, not silently
// truncated to a whole-frame prefix.
// ---------------------------------------------------------------------------
void test_non_frame_aligned_length_rejected() {
    std::string path = unique_socket_path("ack");
    const uint32_t gen = 1;
    RawPeer peer;
    CHECK(peer.start(path, [&](int fd) {
              uint8_t buf[4096];
              raw_recv(fd, buf, sizeof(buf));  // OPEN
              uint8_t ack[LE_AUDIO_SINK_HEADER_BYTES +
                          LE_AUDIO_SINK_OPEN_ACK_PAYLOAD_BYTES];
              build_open_ack(ack, kEpoch, gen);
              raw_send(fd, ack, sizeof(ack));
              raw_recv(fd, buf, sizeof(buf));  // no DATA expected; drain to close
          }),
          "raw peer start");
    EngineSink client;
    CHECK(drive_open(client, path, gen) == 1u, "open");
    std::vector<int16_t> pcm(64 * 2);
    fill_pattern(pcm.data(), 64);
    // 64 frames + 3 trailing bytes: whole frames <= offered, but not a whole
    // number of frames' worth of bytes.
    CHECK(client.on_audio_write(reinterpret_cast<uint8_t*>(pcm.data()),
                                64 * kBytesPerFrame + 3, 1000) == 0u,
          "a non-frame-aligned length is rejected deliberately");
    CHECK(client.last_result() == SinkResult::Malformed,
          "the rejection is an explicit Malformed, not a timeout or a partial credit");
    CHECK(client.accepted_frames() == 0u,
          "a non-frame-aligned length must not silently miscredit a prefix");
    client.disconnect();
    peer.stop();
}

}  // namespace

int main() {
    std::fprintf(stderr,
                 "test_sendspin_ack_validation: reply-validation regressions\n");
    test_finish_ack_epoch();
    test_reset_ack_old_epoch();
    test_credit_accepted_frames();
    test_non_frame_aligned_length_rejected();

    if (libreecho::sendspin::test::g_failures != 0) {
        std::fprintf(stderr,
                     "test_sendspin_ack_validation: %d check(s) FAILED\n",
                     libreecho::sendspin::test::g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_sendspin_ack_validation: all checks passed\n");
    return 0;
}
