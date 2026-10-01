// SPDX-License-Identifier: MIT
//
// Sendspin companion CLI smoke test (C++20).
//
// Runs the built `sendspind` binary as a subprocess against a real in-process
// engine listener (the Platform airplay audio_sink.c) and pins:
//   * --help prints usage and exits 0;
//   * an unknown / incomplete option set is rejected with exit 2 (bounded args);
//   * --check against a live engine opens a generation, verifies readiness and
//     shuts down cleanly with exit 0 and a truthful "ready engine=" line;
//   * --check against a dead socket fails closed with exit 1;
//   * --run in the dependency-free build is refused with exit 2.
//
// The binary path comes from $SENDPIN_COMPANION_BIN (set by the Makefile).

#include "sendspin_engine_fixture.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/wait.h>

using namespace libreecho::sendspin::test;

namespace {

// Runs `cmd`, captures stdout+stderr into *out, returns the exit status.
int run_cmd(const std::string& cmd, std::string* out) {
    std::string piped = cmd + " 2>&1";
    FILE* p = ::popen(piped.c_str(), "r");
    if (p == nullptr) return -1;
    char buf[4096];
    size_t n;
    out->clear();
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out->append(buf, n);
    const int status = ::pclose(p);
    if (status == -1) return -1;
    return WEXITSTATUS(status);
}

std::string shq(const std::string& s) { return std::string("'") + s + "'"; }

}  // namespace

int main() {
    const char* bin_env = std::getenv("SENDPIN_COMPANION_BIN");
    const std::string bin = (bin_env && *bin_env) ? bin_env : "build/sendspind";

    std::string out;

    // --help: usage, exit 0.
    CHECK(run_cmd(shq(bin) + " --help", &out) == 0, "--help exits 0");
    CHECK(out.find("usage: sendspind") != std::string::npos, "--help prints usage");

    // Unknown option: exit 2.
    CHECK(run_cmd(shq(bin) + " --bogus", &out) == 2, "unknown option exits 2");
    CHECK(out.find("unknown option") != std::string::npos,
          "unknown option is reported");

    // No mode: exit 2.
    CHECK(run_cmd(shq(bin), &out) == 2, "missing mode exits 2");

    // Out-of-range bounded arg: exit 2.
    CHECK(run_cmd(shq(bin) + " --check --op-timeout 999999", &out) == 2,
          "out-of-range --op-timeout exits 2");

    // Live engine: --check connects, opens, verifies and shuts down.
    std::string path = unique_socket_path("companion");
    EngineServer server;
    CHECK(server.start(path), "engine server start");
    CHECK(run_cmd(shq(bin) + " --check --socket " + shq(path), &out) == 0,
          "--check against a live engine exits 0");
    CHECK(out.find("ready engine=") != std::string::npos,
          "--check reports truthful readiness");
    CHECK(out.find("epoch=") != std::string::npos && out.find("generation=") != std::string::npos,
          "--check readiness names the engine epoch and generation");
    server.stop();

    // Dead socket: fail closed, exit 1.
    CHECK(run_cmd(shq(bin) + " --check --socket " + shq(path) + "-absent",
                  &out) == 1,
          "--check against a dead socket exits 1");

    // Dependency-free build cannot --run.
    CHECK(run_cmd(shq(bin) + " --run", &out) == 2,
          "--run without the SDK build is refused (exit 2)");

    if (libreecho::sendspin::test::g_failures != 0) {
        std::fprintf(stderr, "test_sendspin_companion: %d check(s) FAILED\n",
                     libreecho::sendspin::test::g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_sendspin_companion: all checks passed\n");
    return 0;
}
