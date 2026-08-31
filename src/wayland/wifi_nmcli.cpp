#include "wayland/wifi_nmcli.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <set>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tdvp::quick_settings {
namespace {

constexpr char kNmcliPath[] = "/usr/bin/nmcli";
constexpr std::size_t kMaximumNmcliOutput = 24U * 1024U;

struct CommandResult {
    int exit_status = 127;
    std::string output;
};

std::vector<std::string> split_terse_fields(const std::string& line)
{
    std::vector<std::string> fields;
    std::string field;
    bool escaped = false;
    for (const char character : line) {
        if (escaped) {
            field += character;
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == ':') {
            fields.push_back(field);
            field.clear();
        } else {
            field += character;
        }
    }
    if (escaped)
        field += '\\';
    fields.push_back(field);
    return fields;
}

int parse_signal_percent(const std::string& value)
{
    int signal = 0;
    for (const char character : value) {
        if (character < '0' || character > '9')
            return 0;
        signal = std::min(100, signal * 10 + (character - '0'));
    }
    return signal;
}

CommandResult run_capture(const std::vector<std::string>& arguments)
{
    CommandResult result;
    if (arguments.empty())
        return result;

    int pipe_fds[2] {};
    if (pipe(pipe_fds) != 0)
        return result;
    const pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return result;
    }
    if (child == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        if (pipe_fds[1] > STDERR_FILENO)
            close(pipe_fds[1]);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1U);
        for (const std::string& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }

    close(pipe_fds[1]);
    std::array<char, 1024> buffer {};
    for (;;) {
        const ssize_t count = read(pipe_fds[0], buffer.data(), buffer.size());
        if (count == 0)
            break;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        const std::size_t available = kMaximumNmcliOutput - result.output.size();
        const std::size_t copied = std::min(available, static_cast<std::size_t>(count));
        result.output.append(buffer.data(), copied);
    }
    close(pipe_fds[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status))
        result.exit_status = WEXITSTATUS(status);
    return result;
}

bool run_detached(const std::vector<std::string>& arguments)
{
    if (arguments.empty())
        return false;
    const pid_t child = fork();
    if (child < 0)
        return false;
    if (child != 0) {
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        return true;
    }
    const pid_t grandchild = fork();
    if (grandchild != 0)
        _exit(grandchild < 0 ? 127 : 0);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (const std::string& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
}

std::set<std::string> saved_wifi_networks()
{
    std::set<std::string> saved;
    const CommandResult result = run_capture(
        {kNmcliPath, "-t", "--escape", "yes", "-f", "NAME,TYPE", "connection", "show"});
    if (result.exit_status != 0)
        return saved;
    std::size_t cursor = 0;
    while (cursor < result.output.size()) {
        const std::size_t line_end = result.output.find('\n', cursor);
        const std::string line = result.output.substr(cursor, line_end == std::string::npos
                                                                   ? std::string::npos
                                                                   : line_end - cursor);
        const std::vector<std::string> fields = split_terse_fields(line);
        if (fields.size() >= 2U && fields[1] == "802-11-wireless" && !fields[0].empty())
            saved.insert(fields[0]);
        if (line_end == std::string::npos)
            break;
        cursor = line_end + 1U;
    }
    return saved;
}

}  // namespace

bool WifiNetwork::requires_password() const
{
    return !security.empty() && security != "--";
}

WifiScanResult scan_wifi_networks()
{
    WifiScanResult scan;
    if (access(kNmcliPath, X_OK) != 0) {
        scan.error = "NetworkManager command is unavailable";
        return scan;
    }
    const CommandResult result = run_capture({kNmcliPath, "-t", "--escape", "yes", "-f",
                                                "IN-USE,SSID,SIGNAL,SECURITY", "device", "wifi",
                                                "list", "--rescan", "no"});
    if (result.exit_status != 0) {
        scan.error = "NetworkManager cannot list Wi-Fi networks";
        return scan;
    }
    const std::set<std::string> saved = saved_wifi_networks();
    std::size_t cursor = 0;
    while (cursor < result.output.size()) {
        const std::size_t line_end = result.output.find('\n', cursor);
        const std::string line = result.output.substr(cursor, line_end == std::string::npos
                                                                   ? std::string::npos
                                                                   : line_end - cursor);
        const std::vector<std::string> fields = split_terse_fields(line);
        if (fields.size() >= 4U && !fields[1].empty() && fields[1] != "--") {
            WifiNetwork network;
            network.active = fields[0] == "*";
            network.ssid = fields[1];
            network.signal_percent = parse_signal_percent(fields[2]);
            network.security = fields[3];
            network.saved = saved.find(network.ssid) != saved.end();
            scan.networks.push_back(std::move(network));
        }
        if (line_end == std::string::npos)
            break;
        cursor = line_end + 1U;
    }
    std::sort(scan.networks.begin(), scan.networks.end(), [](const WifiNetwork& left,
                                                              const WifiNetwork& right) {
        if (left.active != right.active)
            return left.active;
        if (left.signal_percent != right.signal_percent)
            return left.signal_percent > right.signal_percent;
        return left.ssid < right.ssid;
    });
    // A single SSID can have multiple BSSIDs. The sort keeps the active or
    // strongest BSSID first, then the drawer exposes one touch target per
    // human-visible network name.
    scan.networks.erase(std::unique(scan.networks.begin(), scan.networks.end(),
                                    [](const WifiNetwork& left, const WifiNetwork& right) {
                                        return left.ssid == right.ssid;
                                    }),
                        scan.networks.end());
    scan.ok = true;
    return scan;
}

bool request_wifi_rescan()
{
    return run_detached({kNmcliPath, "device", "wifi", "rescan", "ifname", "wlan0"});
}

bool request_wifi_radio(bool enabled)
{
    return run_detached({kNmcliPath, "radio", "wifi", enabled ? "on" : "off"});
}

bool request_wifi_connect(const WifiNetwork& network, const std::string& passphrase)
{
    if (network.ssid.empty())
        return false;
    std::vector<std::string> command {kNmcliPath, "device", "wifi", "connect", network.ssid};
    if (!passphrase.empty()) {
        command.push_back("password");
        command.push_back(passphrase);
    }
    command.push_back("ifname");
    command.push_back("wlan0");
    return run_detached(command);
}

}  // namespace tdvp::quick_settings
