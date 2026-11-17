#include "volume_bridge.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

extern "C" {
#include "../adapter.h"
}

namespace libreecho::sendspin {

VolumeBridge::VolumeBridge(VolumeBridgeConfig config) : config_(std::move(config)) {}

bool VolumeBridge::apply_server_volume(uint8_t volume) {
    if (volume > 100) return false;
    struct le_adapter* audiod = le_adapter_connect(config_.audio_socket.c_str(), config_.timeout_ms);
    if (audiod == nullptr) return false;
    le_adapter_set_io_timeout(audiod, config_.timeout_ms);
    char args[32];
    std::snprintf(args, sizeof(args), "{\"volume\":%u}", static_cast<unsigned>(volume));
    char reply[128];
    const int rc = le_adapter_call(audiod, "set_volume", args, reply, sizeof(reply));
    le_adapter_close(audiod);
    if (rc != LE_ADAPTER_OK) return false;
    last_seen_ = volume;
    return true;
}

std::optional<uint8_t> VolumeBridge::read_master() const {
    const int fd = ::open(config_.master_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return std::nullopt;
    char text[8];
    const ssize_t n = ::read(fd, text, sizeof(text) - 1);
    ::close(fd);
    if (n <= 0) return std::nullopt;
    text[n] = '\0';
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || (*end != '\n' && *end != '\0') || value < 0 || value > 100)
        return std::nullopt;
    return static_cast<uint8_t>(value);
}

std::optional<uint8_t> VolumeBridge::poll_local_change() {
    const auto now = read_master();
    if (!now || now == last_seen_) return std::nullopt;
    last_seen_ = now;
    return now;
}

}  // namespace libreecho::sendspin
