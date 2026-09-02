#include "core/controller.hpp"
#include "core/drawer.hpp"
#include "core/layout.hpp"
#include "core/orientation.hpp"
#include "core/session_power.hpp"
#include "core/status.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using tdvp::quick_settings::AuxiliaryTile;
using tdvp::quick_settings::BackendCommandKind;
using tdvp::quick_settings::Confirmation;
using tdvp::quick_settings::DrawerSettleTarget;
using tdvp::quick_settings::Extent;
using tdvp::quick_settings::GpsState;
using tdvp::quick_settings::HardwareSnapshot;
using tdvp::quick_settings::LteSkuState;
using tdvp::quick_settings::Point;
using tdvp::quick_settings::PrimaryTile;
using tdvp::quick_settings::RequestedAction;
using tdvp::quick_settings::SurfaceTransform;

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

void test_bluetooth_tile_requires_a_real_provider_control()
{
    const HardwareSnapshot enabled = tdvp::quick_settings::parse_status_environment(
        "bluetooth_available=1\n"
        "bluetooth_control_available=1\n"
        "bluetooth_enabled=1\n");
    const auto enabled_model = tdvp::quick_settings::derive_model(enabled);
    EXPECT(enabled.bluetooth_available);
    EXPECT(enabled.bluetooth_control_available);
    EXPECT(enabled.bluetooth_enabled);
    EXPECT(enabled_model.show_bluetooth);
    EXPECT(enabled_model.primary_tile_count == 4U);
    EXPECT(enabled_model.primary_tiles[1] == PrimaryTile::Bluetooth);

    const HardwareSnapshot unavailable = tdvp::quick_settings::parse_status_environment(
        "bluetooth_available=1\n"
        "bluetooth_control_available=0\n");
    const auto unavailable_model = tdvp::quick_settings::derive_model(unavailable);
    EXPECT(!unavailable_model.show_bluetooth);
    EXPECT(unavailable_model.primary_tile_count == 3U);
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
    snapshot.lora_available = true;
    snapshot.bluetooth_available = true;
    snapshot.bluetooth_control_available = true;
    const auto model = tdvp::quick_settings::derive_model(snapshot);
    const auto layout = tdvp::quick_settings::make_layout(Extent {1232, 568}, model);
    EXPECT(layout.supported);
    EXPECT(!layout.requires_vertical_scroll);
    EXPECT(layout.fourth_primary_tile == AuxiliaryTile::Gps);
    EXPECT(tdvp::quick_settings::valid_layout(layout, Extent {1232, 568}));
    EXPECT(layout.primary_card_count == 5U);
    EXPECT(layout.primary_cards[0].width == 163);
    EXPECT(layout.primary_cards[0].height == 100);
    EXPECT(layout.sliders[0].width == 426);
    EXPECT(layout.sliders[2].width == 864);
    EXPECT(!layout.sliders[0].intersects(layout.sliders[2]));
    EXPECT(layout.network_settings.y + layout.network_settings.height == 544);
}

void test_layout_fits_below_the_existing_top_panel()
{
    HardwareSnapshot snapshot;
    const auto model = tdvp::quick_settings::derive_model(snapshot);
    const auto layout = tdvp::quick_settings::make_layout(Extent {1232, 504}, model);
    EXPECT(layout.supported);
    EXPECT(tdvp::quick_settings::valid_layout(layout, Extent {1232, 504}));
    EXPECT(layout.primary_cards[0].y == 64);
    EXPECT(layout.sliders[2].width == 864);
    EXPECT(layout.system_actions[2].y + layout.system_actions[2].height < 504);
    EXPECT(layout.network_settings.y + layout.network_settings.height <= 504);
}

void test_other_logical_modes_are_not_silently_scaled()
{
    HardwareSnapshot snapshot;
    const auto model = tdvp::quick_settings::derive_model(snapshot);
    const auto layout = tdvp::quick_settings::make_layout(Extent {1024, 600}, model);
    EXPECT(!layout.supported);
}

void test_drawer_follows_and_reverses_the_finger()
{
    EXPECT(tdvp::quick_settings::drawer_revealed_from_drag(0.0F, 220.0F, 568.0F) == 220.0F);
    EXPECT(tdvp::quick_settings::drawer_revealed_from_drag(220.0F, -140.0F, 568.0F) == 80.0F);
    EXPECT(tdvp::quick_settings::drawer_revealed_from_drag(80.0F, 800.0F, 568.0F) == 568.0F);
    EXPECT(tdvp::quick_settings::drawer_revealed_from_drag(80.0F, -800.0F, 568.0F) == 0.0F);
}

void test_drawer_release_uses_velocity_then_nearest_state()
{
    EXPECT(tdvp::quick_settings::drawer_settle_target(120.0F, 568.0F, 0.0F) ==
           DrawerSettleTarget::Collapsed);
    EXPECT(tdvp::quick_settings::drawer_settle_target(420.0F, 568.0F, 0.0F) ==
           DrawerSettleTarget::Expanded);
    EXPECT(tdvp::quick_settings::drawer_settle_target(100.0F, 568.0F, 900.0F) ==
           DrawerSettleTarget::Expanded);
    EXPECT(tdvp::quick_settings::drawer_settle_target(480.0F, 568.0F, -900.0F) ==
           DrawerSettleTarget::Collapsed);
}

void test_drawer_settle_starts_without_a_jump()
{
    const float start = 196.0F;
    EXPECT(tdvp::quick_settings::drawer_settle_revealed(start, 568.0F,
                                                         DrawerSettleTarget::Expanded, 0.0F) ==
           start);
    EXPECT(tdvp::quick_settings::drawer_settle_revealed(start, 568.0F,
                                                         DrawerSettleTarget::Expanded, 1.0F) ==
           568.0F);
    EXPECT(tdvp::quick_settings::drawer_settle_duration_ms(start, 568.0F,
                                                            DrawerSettleTarget::Expanded, 0.0F) >=
           90);
}

void test_rotated_k230_surface_uses_landscape_buffer_and_input_coordinates()
{
    const Extent raw_surface {568, 1232};
    const auto buffer = tdvp::quick_settings::buffer_extent_for_surface(
        raw_surface, SurfaceTransform::Rotate270);
    EXPECT(buffer.width == 1232);
    EXPECT(buffer.height == 568);
    const auto top_left = tdvp::quick_settings::surface_to_buffer(
        Point {0, 0}, buffer, SurfaceTransform::Rotate270);
    EXPECT(top_left.x == 1231);
    EXPECT(top_left.y == 0);
    const auto bottom_right = tdvp::quick_settings::surface_to_buffer(
        Point {567, 1231}, buffer, SurfaceTransform::Rotate270);
    EXPECT(bottom_right.x == 0);
    EXPECT(bottom_right.y == 567);
    EXPECT(tdvp::quick_settings::inverse_transform(SurfaceTransform::Rotate90) ==
           SurfaceTransform::Rotate270);
}

void test_session_power_policy_is_bounded_and_touch_cycleable()
{
    const auto parsed = tdvp::quick_settings::parse_session_power_state(
        "lock_after_seconds=120\n"
        "blank_after_seconds=60\n"
        "ignored=value\n"
        "lock_after_seconds=not-a-number\n");
    EXPECT(parsed.lock_after_seconds == 120);
    EXPECT(parsed.blank_after_seconds == 60);

    const auto next_lock = tdvp::quick_settings::next_lock_after(parsed);
    EXPECT(next_lock.lock_after_seconds == 300);
    const auto next_blank = tdvp::quick_settings::next_blank_after(parsed);
    EXPECT(next_blank.blank_after_seconds == 0);
    EXPECT(tdvp::quick_settings::format_session_timeout(300) == "5 min");
    EXPECT(tdvp::quick_settings::format_session_timeout(0) == "Off");
}

}  // namespace

int main()
{
    test_no_lte_keyboard_hides_gps();
    test_lte_keyboard_shows_gps();
    test_wifi_status_is_parsed_without_a_desktop_specific_adapter();
    test_bluetooth_tile_requires_a_real_provider_control();
    test_unsupported_status_never_exposes_gps();
    test_radio_transition_requires_confirmation();
    test_no_lte_keyboard_rejects_gps_control();
    test_target_layout_is_touch_safe_and_scroll_free();
    test_layout_fits_below_the_existing_top_panel();
    test_other_logical_modes_are_not_silently_scaled();
    test_drawer_follows_and_reverses_the_finger();
    test_drawer_release_uses_velocity_then_nearest_state();
    test_drawer_settle_starts_without_a_jump();
    test_rotated_k230_surface_uses_landscape_buffer_and_input_coordinates();
    test_session_power_policy_is_bounded_and_touch_cycleable();
    if (failures == 0) {
        std::cout << "all tdvp-quick-settings core tests passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " tdvp-quick-settings core tests failed\n";
    return EXIT_FAILURE;
}
