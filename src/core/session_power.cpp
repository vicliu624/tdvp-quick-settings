#include "core/session_power.hpp"

#include <array>

namespace tdvp::quick_settings {
namespace {

constexpr int kMaximumTimeoutSeconds = 99999;
constexpr std::array<int, 5> kLockTimeouts {{60, 120, 300, 600, 0}};
constexpr std::array<int, 4> kBlankTimeouts {{15, 30, 60, 0}};

[[nodiscard]] bool parse_seconds(std::string_view value, int* result)
{
    if (value.empty())
        return false;
    int parsed = 0;
    for (const char character : value) {
        if (character < '0' || character > '9')
            return false;
        const int digit = character - '0';
        if (parsed > (kMaximumTimeoutSeconds - digit) / 10)
            return false;
        parsed = parsed * 10 + digit;
    }
    *result = parsed;
    return true;
}

template <std::size_t Size>
[[nodiscard]] int next_value(const std::array<int, Size>& values, int current)
{
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (values[index] == current)
            return values[(index + 1U) % values.size()];
    }
    return values.front();
}

}  // namespace

SessionPowerPolicy parse_session_power_state(std::string_view environment)
{
    SessionPowerPolicy policy;
    std::size_t cursor = 0;
    while (cursor < environment.size()) {
        const std::size_t line_end = environment.find('\n', cursor);
        const std::string_view line = environment.substr(
            cursor, line_end == std::string_view::npos ? std::string_view::npos : line_end - cursor);
        const std::size_t separator = line.find('=');
        if (separator != std::string_view::npos) {
            const std::string_view key = line.substr(0, separator);
            const std::string_view value = line.substr(separator + 1U);
            int seconds = 0;
            if (parse_seconds(value, &seconds)) {
                if (key == "lock_after_seconds")
                    policy.lock_after_seconds = seconds;
                else if (key == "blank_after_seconds")
                    policy.blank_after_seconds = seconds;
            }
        }
        if (line_end == std::string_view::npos)
            break;
        cursor = line_end + 1U;
    }
    return policy;
}

SessionPowerPolicy next_lock_after(SessionPowerPolicy policy)
{
    policy.lock_after_seconds = next_value(kLockTimeouts, policy.lock_after_seconds);
    return policy;
}

SessionPowerPolicy next_blank_after(SessionPowerPolicy policy)
{
    policy.blank_after_seconds = next_value(kBlankTimeouts, policy.blank_after_seconds);
    return policy;
}

std::string format_session_timeout(int seconds)
{
    if (seconds <= 0)
        return "Off";
    if (seconds < 60)
        return std::to_string(seconds) + " sec";
    const int minutes = seconds / 60;
    if (seconds % 60 == 0)
        return std::to_string(minutes) + " min";
    return std::to_string(minutes) + " min " + std::to_string(seconds % 60) + " sec";
}

}  // namespace tdvp::quick_settings
