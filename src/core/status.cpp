#include "core/status.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace tdvp::quick_settings {
namespace {

std::string trim(std::string_view value)
{
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0)
        ++first;
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0)
        --last;
    return std::string(value.substr(first, last - first));
}

bool parse_bool(std::string_view value)
{
    return value == "1" || value == "true" || value == "on" || value == "yes";
}

int parse_percent(std::string_view value)
{
    int result = 0;
    for (const char character : value) {
        if (character < '0' || character > '9')
            return 0;
        result = std::min(100, result * 10 + (character - '0'));
    }
    return result;
}

LteSkuState parse_lte_sku(std::string_view value, bool keyboard_attached)
{
    if (!keyboard_attached)
        return LteSkuState::Detached;
    if (value == "lte-present")
        return LteSkuState::Present;
    if (value == "lte-probing")
        return LteSkuState::Probing;
    if (value == "lte-pending")
        return LteSkuState::Probing;
    if (value == "lte-not-detected" || value == "basic")
        return LteSkuState::NotDetected;
    if (value == "lte-fault")
        return LteSkuState::Fault;
    return LteSkuState::BasicKeyboard;
}

GpsState parse_gps_state(std::string_view value)
{
    if (value == "starting")
        return GpsState::Starting;
    if (value == "searching")
        return GpsState::Searching;
    if (value == "fix")
        return GpsState::Fix;
    if (value == "fault")
        return GpsState::Fault;
    return GpsState::Off;
}

RadioMode parse_radio_mode(std::string_view value)
{
    if (value == "lora")
        return RadioMode::Lora;
    if (value == "nrf9151" || value == "gnss")
        return RadioMode::Gnss;
    return RadioMode::Off;
}

}  // namespace

bool gps_capable(LteSkuState state)
{
    return state == LteSkuState::Present;
}

QuickSettingsModel derive_model(const HardwareSnapshot& snapshot)
{
    QuickSettingsModel model;
    model.show_lora = snapshot.lora_available;
    model.show_gps = gps_capable(snapshot.lte_sku);
    model.gps_interactive = model.show_gps;
    model.fourth_primary_tile = model.show_gps ? AuxiliaryTile::Gps : AuxiliaryTile::OnScreenKeyboard;
    model.lora_detail = snapshot.lora_enabled ? "On" : "Off";
    switch (snapshot.lte_sku) {
    case LteSkuState::Present:
        model.gps_detail = to_string(snapshot.gps_state);
        break;
    case LteSkuState::Probing:
        model.gps_detail = "detecting";
        break;
    case LteSkuState::Fault:
        model.gps_detail = "diagnostic required";
        break;
    default:
        model.gps_detail = "hidden";
        break;
    }
    return model;
}

const char* to_string(LteSkuState state)
{
    switch (state) {
    case LteSkuState::Detached: return "detached";
    case LteSkuState::BasicKeyboard: return "basic-keyboard";
    case LteSkuState::Probing: return "probing";
    case LteSkuState::Present: return "lte-present";
    case LteSkuState::NotDetected: return "lte-not-detected";
    case LteSkuState::Fault: return "lte-fault";
    }
    return "unknown";
}

const char* to_string(GpsState state)
{
    switch (state) {
    case GpsState::Off: return "off";
    case GpsState::Starting: return "starting";
    case GpsState::Searching: return "searching";
    case GpsState::Fix: return "fix";
    case GpsState::Fault: return "fault";
    }
    return "unknown";
}

const char* to_string(RadioMode state)
{
    switch (state) {
    case RadioMode::Off: return "off";
    case RadioMode::Lora: return "lora";
    case RadioMode::Gnss: return "gnss";
    }
    return "unknown";
}

HardwareSnapshot parse_status_environment(std::string_view content)
{
    HardwareSnapshot snapshot;
    std::string lte_value;
    std::size_t offset = 0;
    while (offset < content.size()) {
        const std::size_t line_end = content.find('\n', offset);
        const std::string_view line = content.substr(offset, line_end == std::string_view::npos
                                                                  ? content.size() - offset
                                                                  : line_end - offset);
        const std::size_t separator = line.find('=');
        if (separator != std::string_view::npos) {
            const std::string key = trim(line.substr(0, separator));
            const std::string value = trim(line.substr(separator + 1));
            if (key == "dock_keyboard_input")
                snapshot.keyboard_attached = parse_bool(value);
            else if (key == "dock_profile" && value == "attached")
                snapshot.keyboard_attached = true;
            else if (key == "dock_nrf9151_sku_state")
                lte_value = value;
            else if (key == "wifi_available")
                snapshot.wifi_available = parse_bool(value);
            else if (key == "wifi_enabled" || key == "wifi_radio_enabled")
                snapshot.wifi_enabled = parse_bool(value);
            else if (key == "wifi_link" || key == "wifi_connected")
                snapshot.wifi_connected = parse_bool(value);
            else if (key == "lora_control_available" || key == "lora_available")
                snapshot.lora_available = parse_bool(value);
            else if (key == "lora_requested" || key == "lora_enabled")
                snapshot.lora_enabled = parse_bool(value);
            else if (key == "gps_enabled" || key == "gnss_requested")
                snapshot.gps_enabled = parse_bool(value);
            else if (key == "gps_state" || key == "gnss_state")
                snapshot.gps_state = parse_gps_state(value);
            else if (key == "radio_profile")
                snapshot.radio_mode = parse_radio_mode(value);
            else if (key == "display_brightness_control_available")
                snapshot.display_brightness_available = parse_bool(value);
            else if (key == "keyboard_backlight_available")
                snapshot.keyboard_backlight_available = parse_bool(value);
            else if (key == "display_brightness_percent")
                snapshot.display_brightness_percent = parse_percent(value);
            else if (key == "keyboard_backlight_brightness_percent")
                snapshot.keyboard_backlight_percent = parse_percent(value);
            else if (key == "volume_percent")
                snapshot.volume_percent = parse_percent(value);
            else if (key == "muted")
                snapshot.muted = parse_bool(value);
            else if (key == "speaker_route")
                snapshot.audio_output = value;
        }
        if (line_end == std::string_view::npos)
            break;
        offset = line_end + 1;
    }
    snapshot.lte_sku = parse_lte_sku(lte_value, snapshot.keyboard_attached);
    return snapshot;
}

}  // namespace tdvp::quick_settings
