// SPDX-License-Identifier: MIT
//
// LibreEcho Sendspin companion — src/adapter/sendspind.cpp
//
// The separately-built C++20 companion named by the UI AGENTS.md exception: it
// bridges the pinned Sendspin player SDK to the LibreEcho shared PCM engine over
// the frozen LE_AUDIO_SINK/1 SOCK_SEQPACKET contract.
//
// This program NEVER opens an ALSA/PCM device and never runs a second clock
// servo: the Platform audio engine owns the sole PCM/amplifier device, and the
// companion only feeds the bounded shared sink and reports the engine's own
// timing back to the SDK.
//
// Two build shapes:
//   * dependency-free (no SDK): --help and --check work; --run reports that the
//     SDK is not compiled in. This shape is what the dependency-free opt-in
//     Makefile lane builds and exercises.
//   * full companion (SENDPIN_COMPANION_SDK defined, SDK linked): adds --run,
//     which owns a SendspinClient player role and pumps engine feedback.
//
// No secrets are ever printed or accepted on argv: a seeded identity/PSK store
// belongs to the pairing bootstrap (a later task); this binary never takes key
// material as an argument.

#include "sendspin/engine_sink_session.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <csignal>
#include <sys/un.h>

#ifdef SENDPIN_COMPANION_SDK
#include <chrono>
#include <thread>

#include <sendspin/client.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sendspin/config.h>
#include <sendspin/player_role.h>

#include "sendspin/control_server.h"
#include "sendspin/discovery_lease.h"
#include "sendspin/sdk_persistence.h"
#include "sendspin/sdk_player_listener.h"
#endif

namespace {

using libreecho::sendspin::EngineSinkSession;
using libreecho::sendspin::EngineSinkSessionConfig;
using libreecho::sendspin::SinkResult;
using libreecho::sendspin::kSessionMaxOpTimeoutMs;

constexpr uint32_t kMaxConnectTimeoutMs = 10000;
constexpr uint32_t kMaxPort = 65535;

#ifdef SENDPIN_COMPANION_SDK
volatile sig_atomic_t g_stop = 0;

// Allowance for the engine to start its output from idle (open, prepare,
// amp settle, first period), measured at ~1.42 s on the Echo Dot.
constexpr uint16_t kEngineOutputStartMs = 1500;

// How often the device volume is checked for local changes.
constexpr int kVolumePollMs = 250;

void on_signal(int) { g_stop = 1; }
#endif

void usage(std::FILE* out) {
    std::fprintf(
        out,
        "usage: sendspind [options]\n"
        "\n"
        "LibreEcho Sendspin companion: bridges the Sendspin player SDK to the\n"
        "shared PCM engine over LE_AUDIO_SINK/1 (does not open a PCM device).\n"
        "\n"
        "options:\n"
        "  -h, --help                show this help and exit\n"
        "  --socket PATH             engine sink socket (default %s)\n"
        "  --connect-timeout MS      bounded connect timeout (default 1000, max 10000)\n"
        "  --op-timeout MS           bounded control-op timeout (default 20, max %u)\n"
        "  --max-write-timeout MS    per-write timeout ceiling (default 20, max %u)\n"
        "  --first-generation N      first generation to open (default 1)\n"
        "  --port N                  Sendspin WebSocket port for --run (default 8928)\n"
        "  --name STR                Sendspin display name for --run\n"
        "  --check                   connect/open/verify the engine, then shut down\n"
        "  --run                     run the SDK companion until SIGINT/SIGTERM\n"
        "  --quiet                   suppress the readiness line\n"
        "  --control-socket PATH     local status/pairing socket for the Web UI\n"
        "                            (--run; default /run/libreecho/sendspin.sock)\n"
        "  --state-file PATH         0600 file holding the player identity and\n"
        "                            pairings (--run; without it nothing persists)\n"
        "  --dev-allow-unpaired      DEVELOPMENT ONLY: accept unpaired (Sentinel)\n"
        "                            servers; anyone on the LAN can stream audio\n",
        LE_AUDIO_SINK_DEFAULT_SOCKET_PATH, kSessionMaxOpTimeoutMs,
        kSessionMaxOpTimeoutMs);
}

bool parse_u32(const char* text, uint32_t* out) {
    if (text == nullptr || *text == '\0') return false;
    char* end = nullptr;
    unsigned long v = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || v > 0xfffffffful) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

struct Options {
    EngineSinkSessionConfig session{};
    uint32_t port = 8928;
    std::string name = "LibreEcho Sendspin Companion";
    bool check = false;
    bool run = false;
    bool quiet = false;
    std::string state_file;
    std::string control_socket = "/run/libreecho/sendspin.sock";
    // Development only. Never set by the shipped service definition.
    bool dev_allow_unpaired = false;
};

// Returns 0 on success, 2 on a usage/validation error.
int parse_args(int argc, char** argv, Options* opts) {
    opts->session.socket_path = LE_AUDIO_SINK_DEFAULT_SOCKET_PATH;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto need = [&](const char** out) -> bool {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        const char* value = nullptr;
        if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            usage(stdout);
            std::exit(0);
        } else if (std::strcmp(a, "--socket") == 0) {
            if (!need(&value)) {
                std::fprintf(stderr, "sendspind: --socket needs a PATH\n");
                return 2;
            }
            opts->session.socket_path = value;
        } else if (std::strcmp(a, "--connect-timeout") == 0) {
            if (!need(&value) || !parse_u32(value, &opts->session.connect_timeout_ms) ||
                opts->session.connect_timeout_ms == 0 ||
                opts->session.connect_timeout_ms > kMaxConnectTimeoutMs) {
                std::fprintf(stderr, "sendspind: --connect-timeout must be 1..%u\n",
                             kMaxConnectTimeoutMs);
                return 2;
            }
        } else if (std::strcmp(a, "--op-timeout") == 0) {
            if (!need(&value) || !parse_u32(value, &opts->session.control_timeout_ms) ||
                opts->session.control_timeout_ms == 0 ||
                opts->session.control_timeout_ms > kSessionMaxOpTimeoutMs) {
                std::fprintf(stderr, "sendspind: --op-timeout must be 1..%u\n",
                             kSessionMaxOpTimeoutMs);
                return 2;
            }
        } else if (std::strcmp(a, "--max-write-timeout") == 0) {
            if (!need(&value) ||
                !parse_u32(value, &opts->session.max_write_timeout_ms) ||
                opts->session.max_write_timeout_ms == 0 ||
                opts->session.max_write_timeout_ms > kSessionMaxOpTimeoutMs) {
                std::fprintf(stderr,
                             "sendspind: --max-write-timeout must be 1..%u\n",
                             kSessionMaxOpTimeoutMs);
                return 2;
            }
        } else if (std::strcmp(a, "--first-generation") == 0) {
            if (!need(&value) ||
                !parse_u32(value, &opts->session.first_generation) ||
                opts->session.first_generation == 0) {
                std::fprintf(stderr, "sendspind: --first-generation must be >= 1\n");
                return 2;
            }
        } else if (std::strcmp(a, "--port") == 0) {
            if (!need(&value) || !parse_u32(value, &opts->port) || opts->port == 0 ||
                opts->port > kMaxPort) {
                std::fprintf(stderr, "sendspind: --port must be 1..%u\n", kMaxPort);
                return 2;
            }
        } else if (std::strcmp(a, "--name") == 0) {
            if (!need(&value) || *value == '\0') {
                std::fprintf(stderr, "sendspind: --name needs a value\n");
                return 2;
            }
            opts->name = value;
        } else if (std::strcmp(a, "--check") == 0) {
            opts->check = true;
        } else if (std::strcmp(a, "--run") == 0) {
            opts->run = true;
        } else if (std::strcmp(a, "--quiet") == 0) {
            opts->quiet = true;
        } else if (std::strcmp(a, "--control-socket") == 0 && i + 1 < argc) {
            opts->control_socket = argv[++i];
        } else if (std::strcmp(a, "--state-file") == 0 && i + 1 < argc) {
            opts->state_file = argv[++i];
        } else if (std::strcmp(a, "--dev-allow-unpaired") == 0) {
            opts->dev_allow_unpaired = true;
        } else {
            std::fprintf(stderr, "sendspind: unknown option '%s'\n", a);
            usage(stderr);
            return 2;
        }
    }
    if (opts->check && opts->run) {
        std::fprintf(stderr, "sendspind: --check and --run are mutually exclusive\n");
        return 2;
    }
    if (opts->dev_allow_unpaired && !opts->run) {
        std::fprintf(stderr, "sendspind: --dev-allow-unpaired applies only to --run\n");
        return 2;
    }
    if (!opts->check && !opts->run) {
        std::fprintf(stderr, "sendspind: one of --check or --run is required\n");
        usage(stderr);
        return 2;
    }
    if (opts->session.socket_path.empty() ||
        opts->session.socket_path.size() >= sizeof(((struct sockaddr_un*)nullptr)->sun_path)) {
        std::fprintf(stderr, "sendspind: socket path is empty or too long\n");
        return 2;
    }
    return 0;
}

// Bounded engine readiness probe: connect, open a generation, then shut down
// cleanly. Never leaves a stream armed and never touches a PCM device.
int run_check(const Options& opts) {
    EngineSinkSession session(opts.session);
    // Each op is bounded; resume a timed-out OPEN/FINISH (never resent)
    // within a wall-clock budget. On the Radar an OPEN starts the PCM
    // (mixer arm, amp settle, open, prepare) and the engine answers the
    // following FINISH only after ~1.5 s.
    const auto budget_end =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto in_budget = [&] {
        return std::chrono::steady_clock::now() < budget_end;
    };
    SinkResult r = session.start_stream();
    while (r == SinkResult::Timeout && in_budget()) r = session.start_stream();
    if (r != SinkResult::Ok) {
        std::fprintf(stderr,
                     "sendspind: engine not ready at %s (start_stream -> %d, err=%u)\n",
                     opts.session.socket_path.c_str(), static_cast<int>(r),
                     session.last_error());
        return 1;
    }
    const uint32_t epoch = session.epoch();
    const uint32_t generation = session.generation();
    bool completed = false;
    SinkResult stopped = session.stop_stream(&completed);
    while (stopped == SinkResult::Timeout && in_budget())
        stopped = session.stop_stream(&completed);
    session.disconnect();
    if (stopped != SinkResult::Ok) {
        std::fprintf(stderr,
                     "sendspind: engine accepted OPEN but FINISH failed (-> %d)\n",
                     static_cast<int>(stopped));
        return 1;
    }
    if (!opts.quiet) {
        std::fprintf(stdout,
                     "sendspind: ready engine=%s epoch=%u generation=%u (checked)\n",
                     opts.session.socket_path.c_str(), epoch, generation);
        std::fflush(stdout);
    }
    return 0;
}

#ifdef SENDPIN_COMPANION_SDK
// The SDK starts its WebSocket listener only once a network provider reports
// ready; without one, --run never listens. Ready means an up, non-loopback
// IPv4/IPv6 interface. It is polled only until the listener has started.
struct HostNetworkProvider : ::sendspin::SendspinNetworkProvider {
    bool is_network_ready() override {
        struct ifaddrs* list = nullptr;
        if (::getifaddrs(&list) != 0) return false;
        bool ready = false;
        for (struct ifaddrs* i = list; i != nullptr && !ready; i = i->ifa_next) {
            if (i->ifa_addr == nullptr) continue;
            const int family = i->ifa_addr->sa_family;
            ready = (family == AF_INET || family == AF_INET6) &&
                    (i->ifa_flags & IFF_UP) && !(i->ifa_flags & IFF_LOOPBACK);
        }
        ::freeifaddrs(list);
        return ready;
    }
};

int run_companion(const Options& opts) {
    EngineSinkSession session(opts.session);
    if (session.connect() != SinkResult::Ok) {
        std::fprintf(stderr, "sendspind: cannot connect to engine at %s (err=%u)\n",
                     opts.session.socket_path.c_str(), session.last_error());
        return 1;
    }

    ::sendspin::SendspinClientConfig cfg;
    cfg.name = opts.name;
    cfg.server_port = static_cast<uint16_t>(opts.port);
    if (opts.dev_allow_unpaired) {
        // No persistence provider is registered, so every start is a first
        // boot and this seed takes effect; nothing outlives the process.
        cfg.initial_unpaired_access_enabled = true;
        std::fprintf(stderr,
                     "sendspind: WARNING development mode: unpaired servers accepted; "
                     "any host on the network can stream audio to this device\n");
    }
    // Identity and pairings persist across restarts; corrupt state stops the
    // player rather than silently minting a new identity.
    std::optional<libreecho::sendspin::SdkPersistence> persistence;
    if (!opts.state_file.empty()) {
        std::string error;
        auto store = libreecho::sendspin::BlobStore::open(opts.state_file, &error);
        if (!store) {
            std::fprintf(stderr, "sendspind: state unavailable (%s)\n", error.c_str());
            session.disconnect();
            return 3;
        }
        persistence.emplace(std::move(*store));
        std::string bad;
        if (!persistence->verify_state(cfg.max_pairing_records, &bad)) {
            std::fprintf(stderr,
                         "sendspind: stored Sendspin state '%s' is corrupt; refusing to start "
                         "(unpair or reset Sendspin to recover)\n", bad.c_str());
            session.disconnect();
            return 3;
        }
    }
#ifdef LE_SENDSPIND_CLOCK_TRACE
    // Development builds also log the SDK's per-chunk sync decisions.
    ::sendspin::SendspinClient::set_log_level(::sendspin::LogLevel::VERBOSE);
#endif
    ::sendspin::SendspinClient client(cfg);
    if (persistence) client.set_persistence_provider(&*persistence);
    HostNetworkProvider network;
    client.set_network_provider(&network);

    libreecho::sendspin::SdkPlayerListener listener(session, nullptr);
    libreecho::sendspin::VolumeBridge volume{libreecho::sendspin::VolumeBridgeConfig{}};
    listener.set_volume_bridge(&volume);
    ::sendspin::PlayerRoleConfig player_cfg;
    player_cfg.audio_formats.push_back(
        {::sendspin::SendspinCodecFormat::PCM, 2, 48000, 16});
    // Keep the SDK's default buffer (~1 MB, ~5 s of 48 kHz PCM). The server is
    // asked to stay min_buffer_ms (500 ms) ahead, and that audio must fit the
    // buffer: at 64 KiB (~270 ms advertised) a real server overran it and the
    // SDK dropped most chunks ("Failed to send audio chunk").
    // The server schedules a stream's first chunk only required_lead_time_ms
    // ahead, and drops what the player is not ready for. Starting the shared
    // engine's output from idle takes ~1.4 s on the Echo Dot, almost all of
    // it in the kernel's pcm_prepare() codec power-up, so the SDK's own
    // pipeline lead (150 ms) lost the first second of every stream.
    player_cfg.required_lead_time_ms = static_cast<uint16_t>(
        ::sendspin::PlayerRoleConfig::pipeline_lead_time_ms(
            player_cfg.extra_startup_silence_ms) +
        kEngineOutputStartMs);
    ::sendspin::PlayerRole& player = client.add_player(player_cfg);
    player.set_listener(&listener);
    listener.set_role(&player);
    // The server learns the device's own level; there is no unity fallback.
    if (const auto level = volume.poll_local_change()) player.update_volume(*level);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    if (!client.start()) {
        std::fprintf(stderr, "sendspind: Sendspin client failed to start\n");
        session.disconnect();
        return 1;
    }
    if (!opts.quiet) {
        std::fprintf(stdout,
                     "sendspind: running engine=%s port=%u (SIGINT/SIGTERM to stop)\n",
                     opts.session.socket_path.c_str(), opts.port);
        std::fflush(stdout);
    }

    // Advertised as _sendspin._tcp through the shared supervisor, only while
    // the WebSocket listener is up.
    libreecho::sendspin::DiscoveryConfig discovery_cfg;
    discovery_cfg.port = static_cast<uint16_t>(opts.port);
    discovery_cfg.name = opts.name;
    libreecho::sendspin::DiscoveryLease discovery(discovery_cfg);

    // Status and the pairing token for the authenticated Web UI only.
    libreecho::sendspin::ControlServer control;
    if (!opts.control_socket.empty() && !control.open(opts.control_socket))
        std::fprintf(stderr, "sendspind: control socket %s unavailable\n",
                     opts.control_socket.c_str());
    const auto handle_control = [&](const std::string& command, const char*,
                                    std::string* error) -> std::optional<std::string> {
        using libreecho::sendspin::json_string;
        if (command == "status") {
            const bool connected = client.is_connected();
            const auto server = connected ? client.get_server_information() : std::nullopt;
            std::string data = "{";
            data += "\"listening\":";
            data += libreecho::sendspin::DiscoveryLease::port_listening("/proc/net", discovery_cfg.port)
                        ? "true" : "false";
            data += ",\"discoverable\":";
            data += discovery.accepted() ? "true" : "false";
            data += ",\"connected\":";
            data += connected ? "true" : "false";
            data += ",\"trust\":";
            data += !connected ? "null"
                    : client.get_current_trust() == ::sendspin::ConnectionTrust::USER
                        ? "\"paired\"" : "\"unpaired\"";
            data += ",\"server\":";
            data += server ? json_string(server->name) : "null";
            data += ",\"streaming\":";
            data += session.streaming() ? "true" : "false";
            data += ",\"time_synced\":";
            data += client.is_time_synced() ? "true" : "false";
            data += ",\"volume\":" + std::to_string(player.get_volume());
            data += ",\"muted\":";
            data += player.get_muted() ? "true" : "false";
            data += ",\"client_id\":" + json_string(client.client_id());
            return data + "}";
        }
        if (command == "pairing_token") {
            const auto token = client.pairing_token();
            if (!token) {
                *error = "pairing token unavailable";
                return std::nullopt;
            }
            return "{\"token\":" + json_string(*token) + "}";
        }
        return std::nullopt;
    };

    auto next_volume_poll = std::chrono::steady_clock::now();
    while (!g_stop) {
        client.loop();
        listener.pump_feedback();
        control.poll(handle_control);
        // Local volume changes (buttons, web UI, AirPlay) reach the server.
        if (std::chrono::steady_clock::now() >= next_volume_poll) {
            next_volume_poll += std::chrono::milliseconds(kVolumePollMs);
            if (const auto level = volume.poll_local_change()) player.update_volume(*level);
            discovery.poll(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()));
#ifdef LE_SENDSPIND_CLOCK_TRACE
            // Development-only (never in release builds): the time filter's
            // server->client map, sampled at two points (it is linear), and the
            // last played-frame report, all on CLOCK_MONOTONIC (steady_clock).
            if (client.is_time_synced()) {
                if (FILE* log = std::fopen("/tmp/le-ss/clock-trace.log", "a")) {
                    const long long mono = static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                    std::fprintf(log, "mono_us=%lld c0=%lld c1=%lld reported=%llu report_us=%lld\n",
                                 mono, static_cast<long long>(client.get_client_time(0)),
                                 static_cast<long long>(client.get_client_time(1000000000000LL)),
                                 static_cast<unsigned long long>(listener.reported_frames()),
                                 static_cast<long long>(listener.last_report_timestamp_us()));
                    std::fclose(log);
                }
            }
#endif
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    discovery.withdraw();
    client.stop();
    session.disconnect();
    if (!opts.quiet) {
        std::fprintf(stdout, "sendspind: stopped\n");
        std::fflush(stdout);
    }
    return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    const int parsed = parse_args(argc, argv, &opts);
    if (parsed != 0) return parsed;

    if (opts.check) return run_check(opts);

    // --run
#ifdef SENDPIN_COMPANION_SDK
    return run_companion(opts);
#else
    std::fprintf(stderr,
                 "sendspind: this build has no Sendspin SDK; only --check is "
                 "available (build the companion via the opt-in CMake target)\n");
    return 2;
#endif
}
