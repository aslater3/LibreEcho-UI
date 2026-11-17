#include "control_server.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" {
#include "../adapter.h"
}

namespace libreecho::sendspin {

namespace {
constexpr int kBatch = 8;
constexpr size_t kMessageMax = 4096;
}  // namespace

std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                out += escaped;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

ControlServer::~ControlServer() { close(); }

bool ControlServer::open(const std::string& path) {
    close();
    fd_ = le_adapter_listen(path.c_str());
    if (fd_ < 0) return false;
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        close();
        return false;
    }
    path_ = path;
    return true;
}

void ControlServer::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        ::unlink(path_.c_str());
    }
    fd_ = -1;
}

void ControlServer::poll(const ControlHandler& handler) {
    for (int served = 0; fd_ >= 0 && served < kBatch; ++served) {
        const int client = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) return;
        timeval timeout{0, 200000};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        char message[kMessageMax], response[kMessageMax];
        const ssize_t n = ::read(client, message, sizeof(message) - 1);
        if (n > 0) {
            message[n] = '\0';
            char command[64];
            char* args = nullptr;
            unsigned long id = 0;
            int length;
            if (le_adapter_parse_request(message, command, sizeof(command), &args, &id) < 0) {
                length = le_adapter_respond_err(response, sizeof(response), 0, "malformed request");
            } else {
                std::string error;
                const auto data = handler(command, args, &error);
                length = data ? le_adapter_respond_ok(response, sizeof(response), id, data->c_str())
                              : le_adapter_respond_err(response, sizeof(response), id,
                                                       error.empty() ? "unknown command"
                                                                     : error.c_str());
            }
            if (length > 0) (void)!::write(client, response, static_cast<size_t>(length));
        }
        ::close(client);
    }
}

}  // namespace libreecho::sendspin
