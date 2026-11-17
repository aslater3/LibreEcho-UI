// SPDX-License-Identifier: MIT
//
// Sendspin native bridge — engine-sink generation lifecycle tests (C++20).
//
// Pins the Task 5 lifecycle separation against the real compiled engine:
//   * start -> natural FINISH (no false completion without a physical playhead);
//   * seek-clear / role-removal CANCEL fences the generation and requires a
//     strictly higher OPEN before any further audio is accepted;
//   * no stale ACK / stale generation can permit audio after a fence;
//   * engine restart yields a fresh epoch and rejects the predecessor's
//     generation;
//   * disconnect fences the current generation.

#include "engine_sink.h"
#include "sendspin_engine_fixture.h"

#include <cstdint>
#include <string>
#include <vector>

using libreecho::sendspin::EngineSink;
using libreecho::sendspin::EngineSinkTiming;
using libreecho::sendspin::SinkResult;
using namespace libreecho::sendspin::test;

namespace {

size_t write_frames(EngineSink& client, size_t frames, uint32_t timeout_ms) {
    std::vector<int16_t> pcm(frames * 2);
    fill_pattern(pcm.data(), frames);
    return client.on_audio_write(reinterpret_cast<uint8_t*>(pcm.data()),
                                 frames * kBytesPerFrame, timeout_ms);
}

void test_natural_finish_no_fake_completion() {
    std::string path = unique_socket_path("life");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSink client;
    CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
    CHECK(client.open(1, 2000) == SinkResult::Ok, "open");

    const size_t frames = 512;
    CHECK(write_frames(client, frames, 2000) == frames * kBytesPerFrame,
          "source accepted");

    bool completed = true;
    CHECK(client.finish(frames, 2000, &completed) == SinkResult::Ok,
          "FINISH accepted");
    CHECK(completed == false,
          "no completion without a physical playhead (no fabricated finish)");

    EngineSinkTiming timing{};
    CHECK(client.progress(&timing, 1000) == SinkResult::Ok, "progress ok");
    CHECK(!timing.valid, "timing stays invalid with no DAC model");
    CHECK(timing.finish_us == 0u, "no fabricated finish estimate");

    // DATA after FINISH is refused by the client (disarmed) and never counted.
    CHECK(write_frames(client, 128, 1000) == 0u,
          "no DATA accepted after a natural FINISH");
    CHECK(client.accepted_frames() == frames,
          "post-FINISH refusal left accepted progress untouched");
    CHECK(!client.open_generation(), "client disarmed after FINISH");

    client.disconnect();
    server.stop();
}

void test_seek_clear_cancel_fences_and_reopen_accepts() {
    std::string path = unique_socket_path("life");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSink client;
    CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
    CHECK(client.open(1, 2000) == SinkResult::Ok, "open gen 1");
    CHECK(write_frames(client, 256, 2000) == 256 * kBytesPerFrame,
          "gen 1 source accepted");

    // Seek-clear: cancel with frames still queued on the engine, fence gen 1.
    CHECK(client.cancel(2000) == SinkResult::Ok, "seek-clear cancel ok");
    CHECK(server.sink() != nullptr, "engine still alive after cancel");
    struct le_audio_sink_progress progress;
    CHECK(le_audio_sink_get_progress(server.sink(), &progress) ==
              LE_AUDIO_SINK_OK,
          "post-cancel progress");
    CHECK(!progress.active, "fenced generation is not active");
    CHECK(progress.queued_frames == 0u, "fenced generation queue cleared");

    // No stale ACK can permit audio: before the successor OPEN the client
    // accepts nothing (and sends nothing).
    CHECK(write_frames(client, 256, 1000) == 0u,
          "no audio accepted between fence and successor OPEN");
    CHECK(client.accepted_frames() == 0u, "accepted reset for the successor");

    // A strictly higher generation opens and accepts audio from frame zero.
    CHECK(client.open(2, 2000) == SinkResult::Ok, "open gen 2");
    CHECK(client.generation() == 2u, "successor generation adopted");
    CHECK(write_frames(client, 256, 2000) == 256 * kBytesPerFrame,
          "successor source accepted");
    CHECK(le_audio_sink_get_progress(server.sink(), &progress) ==
              LE_AUDIO_SINK_OK,
          "successor progress");
    CHECK(progress.active && progress.accepted_frames == 256u,
          "engine accepted the successor's frames");

    client.disconnect();
    server.stop();
}

void test_role_removal_before_data() {
    std::string path = unique_socket_path("life");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSink client;
    CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
    CHECK(client.open(5, 2000) == SinkResult::Ok, "open");
    // Role removed before any DATA: cancel must still fence cleanly.
    CHECK(client.cancel(2000) == SinkResult::Ok, "cancel before data ok");

    // Regressing to a generation <= the fenced one is refused locally by
    // open()'s stale-generation guard before any datagram is sent (the real
    // engine's own rejection of a regressed generation is covered separately
    // by the Platform audio_sink suite).
    CHECK(client.open(5, 1000) == SinkResult::Rejected,
          "regressed generation refused");
    CHECK(client.last_error() == LE_AUDIO_SINK_ERR_STALE_GENERATION,
          "stale-generation rejection surfaced");
    // A higher generation recovers.
    CHECK(client.open(6, 2000) == SinkResult::Ok, "recover with gen 6");

    client.disconnect();
    server.stop();
}

void test_engine_restart_new_epoch_rejects_predecessor() {
    std::string path = unique_socket_path("life");

    EngineServer server_a;
    CHECK(server_a.start(path), "server A start");
    const uint32_t epoch_a = server_a.epoch();

    EngineSink client;
    CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect A");
    CHECK(client.open(1, 2000) == SinkResult::Ok, "open A gen 1");
    CHECK(write_frames(client, 128, 2000) == 128 * kBytesPerFrame,
          "A gen 1 audio accepted");

    // Engine restart: the listener dies and is rebound with a fresh epoch.
    client.disconnect();
    server_a.stop();
    server_a.pump(1);

    EngineServer server_b;
    CHECK(server_b.start(path), "server B start");
    CHECK(server_b.epoch() != epoch_a, "restarted engine has a fresh epoch");

    CHECK(client.connect(path, 2000) == SinkResult::Ok, "reconnect B");
    CHECK(client.open(2, 2000) == SinkResult::Ok, "open B gen 2");
    CHECK(client.epoch() == server_b.epoch(), "adopted the restarted epoch");
    CHECK(client.epoch() != epoch_a, "predecessor epoch is stale");
    CHECK(client.accepted_frames() == 0u, "no predecessor progress carried over");
    CHECK(write_frames(client, 128, 2000) == 128 * kBytesPerFrame,
          "audio accepted on the restarted engine");

    client.disconnect();
    server_b.stop();
}

void test_disconnect_fences_current_generation() {
    std::string path = unique_socket_path("life");
    EngineServer server;
    CHECK(server.start(path), "server start");

    EngineSink client;
    CHECK(client.connect(path, 2000) == SinkResult::Ok, "connect");
    CHECK(client.open(1, 2000) == SinkResult::Ok, "open");
    CHECK(write_frames(client, 64, 2000) == 64 * kBytesPerFrame, "audio");

    client.disconnect();
    CHECK(!client.connected(), "disconnected");
    // With no socket, no audio is ever accepted.
    CHECK(write_frames(client, 64, 500) == 0u,
          "no audio accepted after disconnect");

    server.stop();
}

}  // namespace

int main() {
    std::fprintf(stderr,
                 "test_sendspin_lifecycle: generation lifecycle vs real engine\n");
    test_natural_finish_no_fake_completion();
    test_seek_clear_cancel_fences_and_reopen_accepts();
    test_role_removal_before_data();
    test_engine_restart_new_epoch_rejects_predecessor();
    test_disconnect_fences_current_generation();

    if (g_failures != 0) {
        std::fprintf(stderr, "test_sendspin_lifecycle: %d check(s) FAILED\n",
                     g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_sendspin_lifecycle: all checks passed\n");
    return 0;
}
