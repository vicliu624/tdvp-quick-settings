#include "wayland/provider_client.hpp"

#include "core/status.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace tdvp::quick_settings {
namespace {

constexpr char kSocketPath[] = "/run/vicliu-pocket-linux-hardware/quick-settings.sock";
constexpr char kStatusPath[] = "/run/vicliu-pocket-linux-hardware/status.env";
constexpr std::size_t kMaximumReplyBytes = 16384;

std::string trim(std::string value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

ProviderReply parse_reply(const std::string& payload)
{
    ProviderReply reply;
    std::istringstream lines(payload);
    std::string line;
    std::string state_payload;
    while (std::getline(lines, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, separator));
        const std::string value = trim(line.substr(separator + 1));
        if (key == "result") {
            reply.ok = value == "ok";
        } else if (key == "error") {
            reply.error = value;
        } else {
            state_payload += key;
            state_payload += '=';
            state_payload += value;
            state_payload += '\n';
        }
    }
    if (reply.ok)
        reply.snapshot = parse_status_environment(state_payload);
    if (!reply.ok && reply.error.empty())
        reply.error = "hardware provider rejected the request";
    return reply;
}

ProviderReply read_cached_state()
{
    ProviderReply reply;
    std::ifstream status(kStatusPath);
    if (!status) {
        reply.error = "hardware provider is unavailable";
        return reply;
    }
    std::ostringstream contents;
    contents << status.rdbuf();
    reply.ok = true;
    reply.snapshot = parse_status_environment(contents.str());
    return reply;
}

}  // namespace

ProviderReply ProviderClient::state() const
{
    return request("GET_STATE");
}

ProviderReply ProviderClient::request(const std::string& command) const
{
    if (command.empty() || command.size() > 512U) {
        ProviderReply reply;
        reply.error = "invalid provider request";
        return reply;
    }

    const int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (descriptor < 0)
        return command == "GET_STATE" ? read_cached_state() : ProviderReply {false, {}, "cannot open hardware provider socket"};

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (std::strlen(kSocketPath) >= sizeof(address.sun_path)) {
        close(descriptor);
        return ProviderReply {false, {}, "hardware provider socket path is invalid"};
    }
    std::strcpy(address.sun_path, kSocketPath);
    if (connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close(descriptor);
        return command == "GET_STATE" ? read_cached_state() : ProviderReply {false, {}, "hardware provider is unavailable"};
    }

    const std::string packet = command + "\n";
    if (send(descriptor, packet.data(), packet.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(packet.size())) {
        close(descriptor);
        return ProviderReply {false, {}, "hardware provider did not accept the request"};
    }

    std::array<char, kMaximumReplyBytes> buffer {};
    const ssize_t received = recv(descriptor, buffer.data(), buffer.size() - 1U, 0);
    close(descriptor);
    if (received <= 0)
        return ProviderReply {false, {}, "hardware provider returned no response"};
    return parse_reply(std::string(buffer.data(), static_cast<std::size_t>(received)));
}

}  // namespace tdvp::quick_settings
