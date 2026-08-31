#pragma once

#include "core/state.hpp"

#include <array>
#include <cstddef>

namespace tdvp::quick_settings {

enum class BackendCommandKind {
    SetRadioProfileLora,
    SetRadioProfileNrf9151,
    DisableRadio,
    SetLoraPowerOn,
    SetLoraPowerOff,
    SetGnssPowerOn,
    SetGnssPowerOff,
};

struct BackendCommand {
    BackendCommandKind kind;
};

struct CommandList {
    std::array<BackendCommand, 3> values {};
    std::size_t size = 0;

    void push(BackendCommandKind kind);
};

enum class RequestedAction {
    EnableLora,
    DisableLora,
    EnableGps,
    DisableGps,
};

enum class Confirmation {
    None,
    StopGpsBeforeEnablingLora,
    StopLoraBeforeEnablingGps,
};

struct ActionOutcome {
    bool accepted = false;
    Confirmation confirmation = Confirmation::None;
    CommandList commands;
};

[[nodiscard]] ActionOutcome plan_action(const HardwareSnapshot& snapshot,
                                         RequestedAction action,
                                         bool confirmed = false);

}  // namespace tdvp::quick_settings

