/*
 * Sendspin companion — device volume bridge.
 *
 * Sendspin volume is the device's own master volume: Music Assistant stays
 * connected and acts as a remote for the speaker, so a server volume change
 * goes through audiod's set_volume (the same path as the buttons and the web
 * UI) and a local change is reported back to the server. There is no
 * restoration at stream end, unlike an AirPlay sender's temporary level.
 * audiod stays the only volume authority; this bridge never writes the mixer
 * or master.volume itself.
 *
 * Echo suppression: the value this bridge applies is remembered, so audiod
 * publishing it to master.volume is not reported back as a local change.
 *
 * No Sendspin SDK dependency; exercised by the dependency-free companion lane.
 */
#ifndef LIBREECHO_SENDSPIN_VOLUME_BRIDGE_H
#define LIBREECHO_SENDSPIN_VOLUME_BRIDGE_H

#include <cstdint>
#include <optional>
#include <string>

namespace libreecho::sendspin {

struct VolumeBridgeConfig {
    std::string audio_socket = "/run/libreecho/audio.sock";
    std::string master_path = "/run/libreecho-audio/master.volume";
    int timeout_ms = 500;  // one bounded audiod call on the main loop
};

class VolumeBridge {
public:
    explicit VolumeBridge(VolumeBridgeConfig config);

    // Apply a server volume (0-100) through audiod. Returns false, changing
    // nothing, when audiod refuses or is unreachable.
    bool apply_server_volume(uint8_t volume);

    // The device volume when it differs from the last value seen or applied
    // here (the first successful read always reports); nullopt otherwise or
    // while master.volume is missing or malformed.
    std::optional<uint8_t> poll_local_change();

    // Forget the last seen level so the next poll reports the device level
    // (after a refused server volume, the server must learn the real one).
    void forget() { last_seen_.reset(); }

    // Current published device volume, or nullopt.
    std::optional<uint8_t> read_master() const;

private:
    VolumeBridgeConfig config_;
    std::optional<uint8_t> last_seen_;
};

}  // namespace libreecho::sendspin

#endif
