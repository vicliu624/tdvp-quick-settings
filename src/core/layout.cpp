#include "core/layout.hpp"

#include <algorithm>

namespace tdvp::quick_settings {
namespace {

constexpr int kTargetWidth = 1232;
constexpr int kTargetHeight = 568;
constexpr int kStatusHeight = 48;
constexpr int kOuterMargin = 24;
constexpr int kMainWidth = 864;
constexpr int kNetworkX = 928;
constexpr int kNetworkWidth = 280;
constexpr int kCardGap = 12;
constexpr int kPrimaryY = 112;
constexpr int kPrimaryHeight = 100;
constexpr int kSliderY = 223;
constexpr int kSliderHeight = 109;
constexpr int kSecondaryY = 344;
constexpr int kSecondaryHeight = 60;
constexpr int kSystemY = 416;
constexpr int kSystemHeight = 56;

Rect row_rect(int x, int y, int width, int height)
{
    return Rect {x, y, width, height};
}

}  // namespace

bool Rect::contains(int point_x, int point_y) const
{
    return point_x >= x && point_y >= y && point_x < x + width && point_y < y + height;
}

bool Rect::fits_within(int width_limit, int height_limit) const
{
    return x >= 0 && y >= 0 && width >= 0 && height >= 0 && x + width <= width_limit &&
           y + height <= height_limit;
}

bool Rect::intersects(const Rect& other) const
{
    return x < other.x + other.width && x + width > other.x && y < other.y + other.height &&
           y + height > other.y;
}

QuickSettingsLayout make_layout(Extent display, const QuickSettingsModel& model)
{
    QuickSettingsLayout layout;
    layout.fourth_primary_tile = model.fourth_primary_tile;
    if (display.width != kTargetWidth || display.height != kTargetHeight)
        return layout;

    layout.supported = true;
    layout.requires_vertical_scroll = false;
    layout.status_bar = row_rect(0, 0, kTargetWidth, kStatusHeight);
    layout.drawer = row_rect(0, kStatusHeight, kTargetWidth, kTargetHeight - kStatusHeight);

    const int primary_width = (kMainWidth - (3 * kCardGap)) / 4;
    for (int index = 0; index < 4; ++index)
        layout.primary_cards[static_cast<std::size_t>(index)] =
            row_rect(kOuterMargin + index * (primary_width + kCardGap), kPrimaryY, primary_width,
                     kPrimaryHeight);

    const int slider_width = (kMainWidth - (2 * kCardGap)) / 3;
    for (int index = 0; index < 3; ++index)
        layout.sliders[static_cast<std::size_t>(index)] =
            row_rect(kOuterMargin + index * (slider_width + kCardGap), kSliderY, slider_width,
                     kSliderHeight);

    const int secondary_width = (kMainWidth - kCardGap) / 2;
    layout.secondary_actions[0] = row_rect(kOuterMargin, kSecondaryY, secondary_width, kSecondaryHeight);
    layout.secondary_actions[1] =
        row_rect(kOuterMargin + secondary_width + kCardGap, kSecondaryY, secondary_width,
                 kSecondaryHeight);

    const int system_width = (kMainWidth - (2 * kCardGap)) / 3;
    for (int index = 0; index < 3; ++index)
        layout.system_actions[static_cast<std::size_t>(index)] =
            row_rect(kOuterMargin + index * (system_width + kCardGap), kSystemY, system_width,
                     kSystemHeight);

    layout.network_toggle = row_rect(kNetworkX, 112, kNetworkWidth, 44);
    for (int index = 0; index < 4; ++index)
        layout.network_rows[static_cast<std::size_t>(index)] =
            row_rect(kNetworkX, 168 + index * 52, kNetworkWidth, 52);
    layout.network_settings = row_rect(kNetworkX, 492, kNetworkWidth, 52);
    return layout;
}

bool valid_layout(const QuickSettingsLayout& layout, Extent display)
{
    if (!layout.supported || layout.requires_vertical_scroll)
        return false;
    const Rect* groups[] = {
        &layout.status_bar,
        &layout.drawer,
        &layout.network_toggle,
        &layout.network_settings,
    };
    for (const Rect* rect : groups) {
        if (!rect->fits_within(display.width, display.height))
            return false;
    }
    const auto all_fit = [display](const auto& values) {
        return std::all_of(values.begin(), values.end(), [display](const Rect& rect) {
            return rect.fits_within(display.width, display.height) && rect.width >= 48 && rect.height >= 48;
        });
    };
    return all_fit(layout.primary_cards) && all_fit(layout.sliders) && all_fit(layout.secondary_actions) &&
           all_fit(layout.system_actions) && all_fit(layout.network_rows);
}

}  // namespace tdvp::quick_settings

