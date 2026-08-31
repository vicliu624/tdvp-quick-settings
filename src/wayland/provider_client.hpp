#pragma once

#include "core/state.hpp"

#include <string>

namespace tdvp::quick_settings {

// The hardware daemon is the authority for privileged board state.  The UI
// retains no GPIO, mixer-route or radio policy of its own.
struct ProviderReply {
    bool ok = false;
    HardwareSnapshot snapshot;
    std::string error;
};

class ProviderClient {
public:
    [[nodiscard]] ProviderReply state() const;
    [[nodiscard]] ProviderReply request(const std::string& command) const;
};

}  // namespace tdvp::quick_settings
