#pragma once

#include <string>
#include <string_view>

namespace tdvp::quick_settings {

// The policy is deliberately small and session-facing. Authentication belongs
// to PAM/swaylock and output power belongs to wlopm; this type represents only
// the user's idle timing choices.
struct SessionPowerPolicy {
    int lock_after_seconds = 300;
    int blank_after_seconds = 30;
};

[[nodiscard]] SessionPowerPolicy parse_session_power_state(std::string_view environment);
[[nodiscard]] SessionPowerPolicy next_lock_after(SessionPowerPolicy policy);
[[nodiscard]] SessionPowerPolicy next_blank_after(SessionPowerPolicy policy);
[[nodiscard]] std::string format_session_timeout(int seconds);

}  // namespace tdvp::quick_settings
