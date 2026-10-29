// SPDX-License-Identifier: MIT
// Sendspin companion control socket, driven with the UI's real adapter
// client: request dispatch, errors, JSON escaping of untrusted names,
// socket mode, a silent client, and cleanup. No SDK.
#include "control_server.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

extern "C" {
#include "adapter.h"
}

using libreecho::sendspin::ControlServer;
using libreecho::sendspin::json_string;

static int failures = 0;
#define REQUIRE(c, m) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; } } while (0)

int main() {
    REQUIRE(json_string("Kitchen") == "\"Kitchen\"", "plain string");
    REQUIRE(json_string("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"", "escaping");
    REQUIRE(json_string("Küche") == "\"Küche\"", "UTF-8 not passed through");

    char dir_template[] = "/tmp/le-ss-ctl.XXXXXX";
    const std::string dir = ::mkdtemp(dir_template);
    const std::string path = dir + "/sendspin.sock";
    ControlServer server;
    REQUIRE(server.open(path), "control socket open");
    struct stat st{};
    REQUIRE(::stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0660, "socket not 0660");

    std::atomic<bool> stop{false};
    std::thread loop([&] {
        while (!stop) {
            server.poll([](const std::string& cmd, const char* args, std::string* error)
                            -> std::optional<std::string> {
                if (cmd == "status") return std::string("{\"connected\":false}");
                if (cmd == "echo") return std::string("{\"args\":") + args + "}";
                if (cmd == "fail") { *error = "player not ready"; return std::nullopt; }
                return std::nullopt;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    auto call = [&](const char* cmd, const char* args, char* out, size_t size) {
        le_adapter* a = le_adapter_connect(path.c_str(), 1000);
        if (a == nullptr) return -100;
        const int rc = le_adapter_call(a, cmd, args, out, size);
        le_adapter_close(a);
        return rc;
    };
    char out[512];
    REQUIRE(call("status", nullptr, out, sizeof(out)) == LE_ADAPTER_OK &&
            std::strcmp(out, "{\"connected\":false}") == 0, "status");
    REQUIRE(call("echo", "{\"x\":1}", out, sizeof(out)) == LE_ADAPTER_OK &&
            std::strcmp(out, "{\"args\":{\"x\":1}}") == 0, "args not passed");
    REQUIRE(call("fail", nullptr, out, sizeof(out)) == LE_ADAPTER_ERR_REJECTED &&
            std::strcmp(out, "player not ready") == 0, "handler error");
    REQUIRE(call("nope", nullptr, out, sizeof(out)) == LE_ADAPTER_ERR_REJECTED &&
            std::strcmp(out, "unknown command") == 0, "unknown command");

    // A silent client is dropped; the loop keeps serving others.
    const int silent = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::snprintf(a.sun_path, sizeof(a.sun_path), "%s", path.c_str());
    REQUIRE(::connect(silent, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0, "silent connect");
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(call("status", nullptr, out, sizeof(out)) == LE_ADAPTER_OK, "blocked by a silent client");
    REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2), "silent client stalled the loop");
    ::close(silent);

    stop = true;
    loop.join();
    server.close();
    REQUIRE(::access(path.c_str(), F_OK) != 0, "socket not removed on close");
    ::rmdir(dir.c_str());
    if (failures) return 1;
    std::printf("test_sendspin_control: control socket PASS\n");
    return 0;
}
