// SPDX-License-Identifier: MIT
// Sendspin companion volume bridge and source mute, against a fake audiod on a
// real Unix socket and a real master.volume file. No SDK, no hardware.
#include "pcm_mute.h"
#include "volume_bridge.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

using libreecho::sendspin::PcmMute;
using libreecho::sendspin::VolumeBridge;
using libreecho::sendspin::VolumeBridgeConfig;

static int failures = 0;
#define REQUIRE(c, m) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; } } while (0)

// One request per connection, like audiod's adapter clients. Records each
// request line; answers ok, or the configured rejection. On ok, publishes the
// volume to master.volume as audiod does.
struct FakeAudiod {
    std::string socket_path, master_path;
    std::vector<std::string> requests;
    std::atomic<bool> reject{false};
    int listen_fd = -1;
    std::thread thread;

    void start() {
        listen_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::snprintf(a.sun_path, sizeof(a.sun_path), "%s", socket_path.c_str());
        ::unlink(socket_path.c_str());
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0 ||
            ::listen(listen_fd, 4) < 0) std::abort();
    }
    // Serve exactly one connection.
    void serve_one() {
        thread = std::thread([this] {
            const int fd = ::accept(listen_fd, nullptr, nullptr);
            if (fd < 0) return;
            std::string line;
            char c;
            while (::read(fd, &c, 1) == 1 && c != '\n') line.push_back(c);
            requests.push_back(line);
            unsigned long id = 0;
            const auto at = line.find("\"id\":");
            if (at != std::string::npos) id = std::strtoul(line.c_str() + at + 5, nullptr, 10);
            char reply[160];
            if (reject.load()) {
                std::snprintf(reply, sizeof(reply), "{\"v\":1,\"id\":%lu,\"ok\":false,\"error\":\"volume unavailable\"}\n", id);
            } else {
                std::snprintf(reply, sizeof(reply), "{\"v\":1,\"id\":%lu,\"ok\":true,\"data\":{}}\n", id);
                const auto v = line.find("\"volume\":");
                if (v != std::string::npos) {
                    FILE* f = std::fopen(master_path.c_str(), "w");
                    std::fprintf(f, "%ld\n", std::strtol(line.c_str() + v + 9, nullptr, 10));
                    std::fclose(f);
                }
            }
            (void)!::write(fd, reply, std::strlen(reply));
            ::close(fd);
        });
    }
    void join() { if (thread.joinable()) thread.join(); }
};

static void write_master(const std::string& path, const char* text) {
    FILE* f = std::fopen(path.c_str(), "w");
    std::fputs(text, f);
    std::fclose(f);
}

static void test_volume_bridge(const std::string& dir) {
    FakeAudiod audiod;
    audiod.socket_path = dir + "/audio.sock";
    audiod.master_path = dir + "/master.volume";
    audiod.start();
    VolumeBridgeConfig cfg;
    cfg.audio_socket = audiod.socket_path;
    cfg.master_path = audiod.master_path;
    VolumeBridge bridge(cfg);

    // Missing master.volume: nothing to report (no unity/max fallback).
    REQUIRE(!bridge.read_master() && !bridge.poll_local_change(), "missing master reported");
    // First read reports the device level so the server learns it.
    write_master(audiod.master_path, "35\n");
    auto first = bridge.poll_local_change();
    REQUIRE(first && *first == 35, "first device volume not reported");
    REQUIRE(!bridge.poll_local_change(), "unchanged volume reported twice");

    // Server volume goes through audiod set_volume, and is not echoed back.
    audiod.serve_one();
    REQUIRE(bridge.apply_server_volume(60), "server volume not applied");
    audiod.join();
    REQUIRE(audiod.requests.size() == 1 &&
            audiod.requests[0].find("\"cmd\":\"set_volume\"") != std::string::npos &&
            audiod.requests[0].find("\"args\":{\"volume\":60}") != std::string::npos,
            "audiod did not get set_volume 60");
    REQUIRE(!bridge.poll_local_change(), "own server volume echoed back to the server");

    // A local change (buttons, web UI) is reported.
    write_master(audiod.master_path, "42\n");
    auto local = bridge.poll_local_change();
    REQUIRE(local && *local == 42, "local volume change not reported");

    // audiod refuses: false, and nothing is remembered as applied.
    audiod.reject = true;
    audiod.serve_one();
    REQUIRE(!bridge.apply_server_volume(80), "refused volume reported as applied");
    audiod.join();
    REQUIRE(!bridge.poll_local_change(), "refusal changed the reported volume");

    // audiod unreachable: false, bounded.
    ::close(audiod.listen_fd);
    ::unlink(audiod.socket_path.c_str());
    REQUIRE(!bridge.apply_server_volume(10), "unreachable audiod reported success");
    REQUIRE(!bridge.apply_server_volume(101), "out-of-range volume accepted");

    // Malformed master.volume is ignored, never reported as a level.
    for (const char* bad : {"", "abc\n", "101\n", "-1\n", "50x\n"}) {
        write_master(audiod.master_path, bad);
        REQUIRE(!bridge.read_master(), "malformed master.volume accepted");
        REQUIRE(!bridge.poll_local_change(), "malformed master.volume reported");
    }
}

static void test_pcm_mute() {
    const std::vector<int16_t> in(2 * 1000, 10000);
    std::vector<int16_t> out(in.size(), 0);
    PcmMute mute;
    REQUIRE(!mute.apply(in.data(), out.data(), 1000, 2, 0), "unity audio was copied/scaled");

    // Mute at stream frame 500: frames before it stay at full level, then a
    // monotone ramp reaches silence within kRampFrames.
    mute.set_muted(true, 500);
    REQUIRE(mute.apply(in.data(), out.data(), 1000, 2, 0), "muted block passed through");
    REQUIRE(out[0] == 10000 && out[2 * 499] == 10000, "frames before the mute edge altered");
    bool monotone = true;
    for (size_t f = 501; f < 1000; ++f)
        monotone &= out[2 * f] <= out[2 * (f - 1)] && out[2 * f] == out[2 * f + 1];
    REQUIRE(monotone, "mute ramp not monotone or channels differ");
    REQUIRE(out[2 * (500 + PcmMute::kRampFrames)] == 0 && out.back() == 0,
            "mute did not reach silence within the ramp");
    REQUIRE(in[2 * 600] == 10000, "apply() modified its input");

    // An SDK retry of a partially accepted block yields identical bytes.
    std::vector<int16_t> retry(2 * 400, 0);
    mute.apply(in.data(), retry.data(), 400, 2, 600);
    REQUIRE(std::equal(retry.begin(), retry.end(), out.begin() + 2 * 600),
            "retry of the same stream frames produced different PCM");

    // Unmute mid-ramp continues from the current level, not from silence.
    mute.set_muted(false, 520);
    const uint32_t at_edge = mute.gain_at(520);
    REQUIRE(at_edge > 0 && at_edge < PcmMute::kUnity, "unmute edge not mid-ramp");
    REQUIRE(mute.gain_at(521) > at_edge, "unmute did not rise from the edge level");
    REQUIRE(mute.gain_at(520 + PcmMute::kRampFrames) == PcmMute::kUnity,
            "unmute did not restore full level within the ramp");

    // A new stream starts at the target level with no ramp.
    mute.set_muted(true, 2000);
    mute.reset_stream();
    REQUIRE(mute.gain_at(0) == 0, "new stream while muted was audible");
    mute.set_muted(false, 0);
    mute.reset_stream();
    REQUIRE(!mute.apply(in.data(), out.data(), 10, 2, 0), "new unmuted stream was ramped");

    const std::vector<int16_t> full = {-32768, 32767};
    std::vector<int16_t> full_out(2, 0);
    mute.set_muted(true, 0);
    mute.reset_stream();
    mute.set_muted(false, 0);
    // Once the unmute ramp completes the block is unity: pass-through, and a
    // forced scale at unity gain must not alter full-scale samples either.
    REQUIRE(!mute.apply(full.data(), full_out.data(), 1, 2, PcmMute::kRampFrames),
            "completed unmute still scaled audio");
    mute.set_muted(true, 1000000);
    REQUIRE(mute.apply(full.data(), full_out.data(), 1, 2, 0), "muted stream passed through");
    REQUIRE(full_out[0] == -32768 && full_out[1] == 32767, "full-scale samples altered at unity gain");
}

int main() {
    char dir_template[] = "/tmp/le-sendspin-volume.XXXXXX";
    const char* dir = ::mkdtemp(dir_template);
    if (dir == nullptr) return 2;
    test_volume_bridge(dir);
    test_pcm_mute();
    std::string cleanup = std::string("rm -rf ") + dir;
    (void)!std::system(cleanup.c_str());
    if (failures) return 1;
    std::printf("test_sendspin_volume: volume bridge and source mute PASS\n");
    return 0;
}
