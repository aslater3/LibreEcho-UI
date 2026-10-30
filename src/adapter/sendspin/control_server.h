/*
 * Sendspin companion — local control socket for the Web daemon.
 *
 * The same one-line JSON adapter protocol as the other LibreEcho daemons,
 * on a 0660 root socket (default /run/libreecho/sendspin.sock). The Web
 * daemon reads player status and, for the authenticated pairing flow, the
 * pairing token; no other process can, and nothing is logged. One request
 * per connection; the main loop polls it without blocking (a silent client
 * is dropped after a short receive timeout).
 *
 * No Sendspin SDK dependency; the companion supplies the command handler.
 */
#ifndef LIBREECHO_SENDSPIN_CONTROL_SERVER_H
#define LIBREECHO_SENDSPIN_CONTROL_SERVER_H

#include <functional>
#include <optional>
#include <string>

namespace libreecho::sendspin {

// Returns the response's "data" JSON object, or nullopt with *error set.
using ControlHandler = std::function<std::optional<std::string>(
    const std::string& command, const char* args, std::string* error)>;

class ControlServer {
public:
    ControlServer() = default;
    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    bool open(const std::string& path);
    // Serve every pending request (bounded batch); never blocks on accept.
    void poll(const ControlHandler& handler);
    void close();

private:
    int fd_ = -1;
    std::string path_;
};

// A JSON string literal (quoted, escaped) for untrusted text.
std::string json_string(const std::string& text);

}  // namespace libreecho::sendspin

#endif
