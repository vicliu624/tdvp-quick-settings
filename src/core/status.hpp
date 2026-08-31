#pragma once

#include "core/state.hpp"

#include <string_view>

namespace tdvp::quick_settings {

[[nodiscard]] HardwareSnapshot parse_status_environment(std::string_view content);

}  // namespace tdvp::quick_settings

