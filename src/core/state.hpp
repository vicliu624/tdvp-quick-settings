#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace tdvp::quick_settings {

enum class LteSkuState {
    Detached,
    BasicKeyboard,
    Probing,
    Present,
    NotDetected,
    Fault,
};

enum class GpsState {
    Off,
    Starting,
    Searching,
    Fix,
    Fault,
};

enum class RadioMode {
    Off,
    Lora,
    Gnss,
};

enum class AuxiliaryTile {
    Gps,
    OnScreenKeyboard,
};

// The first row is capability-driven: a card exists only when it can perform
// a real action on the current board.  This keeps optional radios from
// becoming decorative, non-functional controls.
enum class PrimaryTile {
    Wifi,
    Bluetooth,
    AudioOutput,
    Lora,
    Gps,
    OnScreenKeyboard,
};

struct HardwareSnapshot {
    bool keyboard_attached = false;
    bool wifi_available = false;
    bool wifi_enabled = false;
    bool wifi_connected = false;
    bool bluetooth_available = false;
    bool bluetooth_control_available = false;
    bool bluetooth_enabled = false;
    bool lora_available = false;
    bool lora_enabled = false;
    LteSkuState lte_sku = LteSkuState::Detached;
    bool gps_enabled = false;
    GpsState gps_state = GpsState::Off;
    RadioMode radio_mode = RadioMode::Off;
    bool display_brightness_available = false;
    bool keyboard_backlight_available = false;
    int display_brightness_percent = 0;
    int keyboard_backlight_percent = 0;
    int volume_percent = 0;
    bool muted = false;
    std::string audio_output = "unknown";
};

struct QuickSettingsModel {
    bool show_lora = false;
    bool show_gps = false;
    bool gps_interactive = false;
    bool show_bluetooth = false;
    std::array<PrimaryTile, 5> primary_tiles {};
    std::size_t primary_tile_count = 0;
    AuxiliaryTile fourth_primary_tile = AuxiliaryTile::OnScreenKeyboard;
    std::string gps_detail;
    std::string lora_detail;
};

[[nodiscard]] bool gps_capable(LteSkuState state);
[[nodiscard]] QuickSettingsModel derive_model(const HardwareSnapshot& snapshot);
[[nodiscard]] const char* to_string(LteSkuState state);
[[nodiscard]] const char* to_string(GpsState state);
[[nodiscard]] const char* to_string(RadioMode state);

}  // namespace tdvp::quick_settings
