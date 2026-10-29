#include "discovery_lease.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <utility>

extern "C" {
#include "../mdns_client.h"
}

namespace libreecho::sendspin {

DiscoveryLease::DiscoveryLease(DiscoveryConfig config)
    : config_(std::move(config)), backoff_ms_(config_.retry_min_ms) {}

DiscoveryLease::~DiscoveryLease() { withdraw(); }

void DiscoveryLease::withdraw() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    accepted_ = false;
}

// A LISTEN (state 0A) socket bound to `port` in /proc/net/tcp or tcp6.
bool DiscoveryLease::port_listening(const std::string& proc_net, uint16_t port) {
    for (const char* table : {"/tcp", "/tcp6"}) {
        FILE* f = std::fopen((proc_net + table).c_str(), "r");
        if (f == nullptr) continue;
        char line[512];
        bool found = false;
        if (std::fgets(line, sizeof(line), f) != nullptr) {  // header
            while (!found && std::fgets(line, sizeof(line), f) != nullptr) {
                char local[64] = {0}, state[8] = {0};
                if (std::sscanf(line, "%*s %63s %*s %7s", local, state) != 2) continue;
                const char* colon = std::strrchr(local, ':');
                if (colon == nullptr) continue;
                found = std::strtoul(colon + 1, nullptr, 16) == port &&
                        std::strcmp(state, "0A") == 0;
            }
        }
        std::fclose(f);
        if (found) return true;
    }
    return false;
}

void DiscoveryLease::poll(uint64_t now_ms) {
    const bool listening = port_listening(config_.proc_net, config_.port);
    if (fd_ >= 0) {
        const int state = listening ? le_mdns_receive(fd_) : -1;
        if (state == 1) accepted_ = true;
        if (state >= 0) return;
        // Rejected, supervisor gone, or our listener stopped: withdraw and retry.
        withdraw();
        next_try_ms_ = now_ms + backoff_ms_;
        backoff_ms_ = std::min(backoff_ms_ * 2, config_.retry_max_ms);
        return;
    }
    if (!listening || now_ms < next_try_ms_) return;
    fd_ = le_mdns_register_sendspin(config_.mdns_socket.c_str(), config_.port,
                                    config_.name.c_str());
    if (fd_ < 0) {
        next_try_ms_ = now_ms + backoff_ms_;
        backoff_ms_ = std::min(backoff_ms_ * 2, config_.retry_max_ms);
        return;
    }
    backoff_ms_ = config_.retry_min_ms;
}

}  // namespace libreecho::sendspin
