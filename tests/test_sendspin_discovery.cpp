// SPDX-License-Identifier: MIT
// Sendspin discovery lease against a scripted mDNS supervisor socket and a
// real TCP listener: registers only while the port listens, re-registers
// after the supervisor drops it, withdraws when the listener goes. No SDK.
#include "discovery_lease.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

using libreecho::sendspin::DiscoveryConfig;
using libreecho::sendspin::DiscoveryLease;

static int failures = 0;
#define REQUIRE(c, m) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; } } while (0)

// Accepts registrations, answers "pending", and holds each until dropped.
struct FakeSupervisor {
    int listen_fd = -1;
    std::mutex mu;
    std::vector<std::string> packets;
    std::vector<int> clients;
    std::atomic<bool> stop{false};
    std::thread thread;

    explicit FakeSupervisor(const std::string& path) {
        listen_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::snprintf(a.sun_path, sizeof(a.sun_path), "%s", path.c_str());
        ::unlink(path.c_str());
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
            ::listen(listen_fd, 4) != 0) std::abort();
        timeval tv{0, 50000};
        ::setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        thread = std::thread([this] {
            while (!stop) {
                const int fd = ::accept(listen_fd, nullptr, nullptr);
                if (fd < 0) continue;
                char buf[256];
                const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                std::lock_guard<std::mutex> g(mu);
                if (n > 0) packets.emplace_back(buf, static_cast<size_t>(n));
                (void)!::send(fd, "pending\n", 8, MSG_NOSIGNAL);
                clients.push_back(fd);
            }
        });
    }
    ~FakeSupervisor() {
        stop = true;
        thread.join();
        drop_all();
        ::close(listen_fd);
    }
    size_t count() { std::lock_guard<std::mutex> g(mu); return packets.size(); }
    std::string last() { std::lock_guard<std::mutex> g(mu); return packets.empty() ? "" : packets.back(); }
    void drop_all() {
        std::lock_guard<std::mutex> g(mu);
        for (const int fd : clients) ::close(fd);
        clients.clear();
    }
    // True once the held registration was closed by the client.
    bool client_gone() {
        std::lock_guard<std::mutex> g(mu);
        for (const int fd : clients) {
            char c;
            const ssize_t n = ::recv(fd, &c, 1, MSG_DONTWAIT);
            if (n != 0) return false;
        }
        return true;
    }
};

static uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static bool settle(DiscoveryLease& lease, const auto& condition, int ms = 2000) {
    const uint64_t end = now_ms() + static_cast<uint64_t>(ms);
    while (now_ms() < end) {
        lease.poll(now_ms());
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

int main() {
    char dir_template[] = "/tmp/le-ss-disc.XXXXXX";
    const std::string dir = ::mkdtemp(dir_template);
    FakeSupervisor supervisor(dir + "/mdns.sock");

    // A real TCP port we control the listening state of.
    const int tcp = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (::bind(tcp, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::getsockname(tcp, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 2;
    const uint16_t port = ntohs(addr.sin_port);

    DiscoveryConfig cfg;
    cfg.mdns_socket = dir + "/mdns.sock";
    cfg.port = port;
    cfg.name = "Küche";
    cfg.retry_min_ms = 50;
    cfg.retry_max_ms = 200;
    {
        DiscoveryLease lease(cfg);
        // Bound but not listening: nothing is advertised.
        for (int i = 0; i < 20; ++i) { lease.poll(now_ms()); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        REQUIRE(!lease.registered() && supervisor.count() == 0, "advertised before the port listened");
        REQUIRE(!DiscoveryLease::port_listening("/proc/net", port), "bound port reported listening");

        ::listen(tcp, 1);
        REQUIRE(DiscoveryLease::port_listening("/proc/net", port), "listening port not found");
        REQUIRE(settle(lease, [&] { return lease.accepted(); }), "not registered once listening");
        char expected[96];
        std::snprintf(expected, sizeof(expected), "SENDSPIN/1 %u name=4bc3bc636865\n", port);
        REQUIRE(supervisor.count() == 1 && supervisor.last() == expected, "wrong registration packet");

        // Supervisor restart (registration dropped): re-registers after backoff.
        supervisor.drop_all();
        REQUIRE(settle(lease, [&] { return supervisor.count() == 2 && lease.accepted(); }),
                "did not re-register after the supervisor dropped it");

        // Listener gone: the record is withdrawn.
        ::close(tcp);
        REQUIRE(settle(lease, [&] { return !lease.registered(); }), "kept advertising a closed port");
        REQUIRE(settle(lease, [&] { return supervisor.client_gone(); }), "registration not closed");
        for (int i = 0; i < 20; ++i) { lease.poll(now_ms()); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        REQUIRE(supervisor.count() == 2, "re-registered without a listener");
    }
    const std::string cleanup = "rm -rf " + dir;
    (void)!std::system(cleanup.c_str());
    if (failures) return 1;
    std::printf("test_sendspin_discovery: listener-gated mDNS lease PASS\n");
    return 0;
}
