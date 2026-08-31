#include "core/controller.hpp"
#include "core/layout.hpp"
#include "core/status.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using tdvp::quick_settings::AuxiliaryTile;
using tdvp::quick_settings::BackendCommandKind;
using tdvp::quick_settings::Confirmation;
using tdvp::quick_settings::Extent;
using tdvp::quick_settings::GpsState;
using tdvp::quick_settings::HardwareSnapshot;
using tdvp::quick_settings::LteSkuState;
using tdvp::quick_settings::RequestedAction;

int failures = 0;

void expect(bool condition, const char* expression, const char* file, int line)
{
    if (condition)
        return;
    std::cerr << file << ':' << line << ": expectation failed: " << expression << '\n';
    ++failures;
}

#define EXPECT(expression) expect((expression), #expression, __FILE__, __LINE__)

void test_no_lte_keyboard_hides_gps()
{
    const HardwareSnapshot snapshot = tdvp::quick_settings::parse_status_environment(
        "dock_profile=attached\n"
        "dock_keyboard_input=1\n"
        "dock_nrf9151_sku_state=lte-not-detected\n"
        "lora_control_available=1\n"
        "lora_requested=1\n");
    const auto model = tdvp::quick_settings::derive_model(snapshot);
    EXPECT(snapshot.keyboard_attached);
    EXPECT(snapshot.lte_sku == LteSkuState::NotDetected);
    EXPECT(model.show_lora);
    EXPECT(!model.show_gps);
    EXPECT(!model.gps_interactive);
    EXPECT(model.fourth_primary_tile == AuxiliaryTile::OnScreenKeyboard);
}

void test_lte_keyboard_shows_gps()
{
    const HardwareSnapshot snapshot = tdvp::quick_settings::parse_status_environment(
        "dock_profile=attached\n"
        "dock_nrf9151_sku_state=lte-present\n"
        "gps_state=fix\n"
        "lora_control_available=1\n");
    const auto model = tdvp::quick_settings::derive_model(snapshot);
    EXPECT(snapshot.lte_sku == LteSkuState::Present);
    EXPECT(snapshot.gps_state == GpsState::Fix);
    EXPECT(model.show_gps);
    EXPECT(model.gps_interactive);
    EXPECT(model.fourth_primary_tile == AuxiliaryTile::Gps);
}

void test_wifi_status_is_parsed_without_a_desktop_specific_adapter()
{
    const HardwareSnapshot snapshot = tdvp::quick_settings::parse_status_environment(
        "wifi_available=1\n"
        "wifi_enabled=1\n"
        "wifi_link=1\n");
    EXPECT(snapshot.wifi_available);
    EXPECT(snapshot.wifi_enabled);
    EXPECT(snapshot.wifi_connected);
}

void test_unsupported_status_never_exposes_gps()
{
    const HardwareSnapshot snapshot = tdvp::quick_settings::parse_status_environment(
        "dock_profile=attached\n"
        "dock_nrf9151_sku_state=lte-probing\n"
        "gnss_requested=1\n");
    const auto model = tdvp::quick_settings::derive_model(snapshot);
    EXPECT(snapshot.lte_sku == LteSkuState::Probing);
    EXPECT(!model.show_gps);
    EXPECT(model.gps_detail == "detecting");
}

void test_radio_transition_requires_confirmation()
{
    HardwareSnapshot snapshot;
    snapshot.lora_available = true;
    snapshot.lora_enabled = true;
    snapshot.lte_sku = LteSkuState::Present;
    const auto request = tdvp::quick_settings::plan_action(snapshot, RequestedAction::EnableGps);
    EXPECT(!request.accepted);
    EXPECT(request.confirmation == Confirmation::StopLoraBeforeEnablingGps);
    EXPECT(request.commands.size == 0U);

    const auto confirmed = tdvp::quick_settings::plan_action(snapshot, RequestedAction::EnableGps, true);
    EXPECT(confirmed.accepted);
    EXPECT(confirmed.commands.size == 3U);
    EXPECT(confirmed.commands.values[0].kind == BackendCommandKind::SetLoraPowerOff);
    EXPECT(confirmed.commands.values[1].kind == BackendCommandKind::SetRadioProfileNrf9151);
    EXPECT(confirmed.commands.values[2].kind == BackendCommandKind::SetGnssPowerOn);
}

void test_no_lte_keyboard_rejects_gps_control()
{
    HardwareSnapshot snapshot;
    snapshot.lte_sku = LteSkuState::NotDetected;
    const auto result = tdvp::quick_settings::plan_action(snapshot, RequestedAction::EnableGps, true);
    EXPECT(!result.accepted);
    EXPECT(result.confirmation == Confirmation::None);
    EXPECT(result.commands.size == 0U);
}

void test_target_layout_is_touch_safe_and_scroll_free()
{
    HardwareSnapshot snapshot;
    snapshot.lte_sku = LteSkuState::Present;
    const auto model = tdvp::quick_settings::derive_model(snapshot);
    const auto layout = tdvp::quick_settings::make_layout(Extent {1232, 568}, model);
    EXPECT(layout.supported);
    EXPECT(!layout.requires_vertical_scroll);
    EXPECT(layout.fourth_primary_tile == AuxiliaryTile::Gps);
    EXPECT(tdvp::quick_settings::valid_layout(layout, Extent {1232, 568}));
    EXPECT(layout.primary_cards[0].width == 207);
    EXPECT(layout.primary_cards[0].height == 100);
    EXPECT(layout.sliders[0].width == 280);
    EXPECT(layout.network_settings.y + layout.network_settings.height == 544);
}

void test_other_logical_modes_are_not_silently_scaled()
{
    HardwareSnapshot snapshot;
    const auto model = tdvp::quick_settings::derive_model(snapshot);
    const auto layout = tdvp::quick_settings::make_layout(Extent {1024, 600}, model);
    EXPECT(!layout.supported);
}

}  // namespace

int main()
{
    test_no_lte_keyboard_hides_gps();
    test_lte_keyboard_shows_gps();
    test_wifi_status_is_parsed_without_a_desktop_specific_adapter();
    test_unsupported_status_never_exposes_gps();
    test_radio_transition_requires_confirmation();
    test_no_lte_keyboard_rejects_gps_control();
    test_target_layout_is_touch_safe_and_scroll_free();
    test_other_logical_modes_are_not_silently_scaled();
    if (failures == 0) {
        std::cout << "all tdvp-quick-settings core tests passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " tdvp-quick-settings core tests failed\n";
    return EXIT_FAILURE;
}
