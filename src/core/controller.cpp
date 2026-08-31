#include "core/controller.hpp"

#include <cassert>

namespace tdvp::quick_settings {

void CommandList::push(BackendCommandKind kind)
{
    assert(size < values.size());
    values[size++] = BackendCommand {kind};
}

ActionOutcome plan_action(const HardwareSnapshot& snapshot, RequestedAction action, bool confirmed)
{
    ActionOutcome outcome;
    switch (action) {
    case RequestedAction::EnableLora:
        if (!snapshot.lora_available)
            return outcome;
        if (snapshot.gps_enabled && !confirmed) {
            outcome.confirmation = Confirmation::StopGpsBeforeEnablingLora;
            return outcome;
        }
        outcome.accepted = true;
        if (snapshot.gps_enabled)
            outcome.commands.push(BackendCommandKind::SetGnssPowerOff);
        outcome.commands.push(BackendCommandKind::SetRadioProfileLora);
        outcome.commands.push(BackendCommandKind::SetLoraPowerOn);
        return outcome;
    case RequestedAction::DisableLora:
        if (!snapshot.lora_available)
            return outcome;
        outcome.accepted = true;
        outcome.commands.push(BackendCommandKind::SetLoraPowerOff);
        return outcome;
    case RequestedAction::EnableGps:
        if (!gps_capable(snapshot.lte_sku))
            return outcome;
        if (snapshot.lora_enabled && !confirmed) {
            outcome.confirmation = Confirmation::StopLoraBeforeEnablingGps;
            return outcome;
        }
        outcome.accepted = true;
        if (snapshot.lora_enabled)
            outcome.commands.push(BackendCommandKind::SetLoraPowerOff);
        outcome.commands.push(BackendCommandKind::SetRadioProfileNrf9151);
        outcome.commands.push(BackendCommandKind::SetGnssPowerOn);
        return outcome;
    case RequestedAction::DisableGps:
        if (!gps_capable(snapshot.lte_sku))
            return outcome;
        outcome.accepted = true;
        outcome.commands.push(BackendCommandKind::SetGnssPowerOff);
        return outcome;
    }
    return outcome;
}

}  // namespace tdvp::quick_settings

