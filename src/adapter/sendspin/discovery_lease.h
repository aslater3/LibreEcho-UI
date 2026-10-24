/*
 * Sendspin companion — mDNS advertisement through the shared supervisor.
 *
 * The player is discovered as _sendspin._tcp (spec connection.md). The shared
 * libreecho-mdnsd owns Avahi and publishes the record only while this process
 * holds its registration connection, so a crash withdraws it. This class:
 *
 *   * registers only once the WebSocket port is really listening (read from
 *     /proc/net/tcp{,6}; no probe connection is made to the player),
 *   * re-registers with backoff when the supervisor drops or rejects it
 *     (for example after it restarts),
 *   * withdraws when the listener goes away or on destruction.
 *
 * No Sendspin SDK dependency; exercised by the dependency-free companion lane.
 */
#ifndef LIBREECHO_SENDSPIN_DISCOVERY_LEASE_H
#define LIBREECHO_SENDSPIN_DISCOVERY_LEASE_H

#include <cstdint>
#include <string>

namespace libreecho::sendspin {

struct DiscoveryConfig {
    std::string mdns_socket = "/run/libreecho/mdns.sock";
    uint16_t port = 8928;
    std::string name;
    std::string proc_net = "/proc/net";  // tcp and tcp6 tables
    uint64_t retry_min_ms = 2000;
    uint64_t retry_max_ms = 30000;
};

class DiscoveryLease {
public:
    explicit DiscoveryLease(DiscoveryConfig config);
    ~DiscoveryLease();
    DiscoveryLease(const DiscoveryLease&) = delete;
    DiscoveryLease& operator=(const DiscoveryLease&) = delete;

    // Advance the lease; call periodically from the main loop.
    void poll(uint64_t now_ms);
    void withdraw();

    bool registered() const { return fd_ >= 0; }
    bool accepted() const { return accepted_; }  // supervisor acknowledged
    static bool port_listening(const std::string& proc_net, uint16_t port);

private:
    DiscoveryConfig config_;
    int fd_ = -1;
    bool accepted_ = false;
    uint64_t next_try_ms_ = 0;
    uint64_t backoff_ms_;
};

}  // namespace libreecho::sendspin

#endif
