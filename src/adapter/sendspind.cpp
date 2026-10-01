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
#include <sendspin/config.h>
#include <sendspin/player_role.h>

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
        "  --quiet                   suppress the readiness line\n",
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
    SinkResult r = session.start_stream();
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
    const SinkResult stopped = session.stop_stream(&completed);
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
    ::sendspin::SendspinClient client(cfg);

    libreecho::sendspin::SdkPlayerListener listener(session, nullptr);
    ::sendspin::PlayerRoleConfig player_cfg;
    player_cfg.audio_formats.push_back(
        {::sendspin::SendspinCodecFormat::PCM, 2, 48000, 16});
    player_cfg.audio_buffer_capacity = 64 * 1024;
    ::sendspin::PlayerRole& player = client.add_player(player_cfg);
    player.set_listener(&listener);
    listener.set_role(&player);

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

    while (!g_stop) {
        client.loop();
        listener.pump_feedback();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

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
