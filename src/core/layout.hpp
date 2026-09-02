#pragma once

#include "core/state.hpp"

#include <array>

namespace tdvp::quick_settings {

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] bool contains(int point_x, int point_y) const;
    [[nodiscard]] bool fits_within(int width_limit, int height_limit) const;
    [[nodiscard]] bool intersects(const Rect& other) const;
};

struct Extent {
    int width = 0;
    int height = 0;
};

struct QuickSettingsLayout {
    bool supported = false;
    bool requires_vertical_scroll = false;
    Rect status_bar;
    Rect drawer;
    std::array<Rect, 5> primary_cards {};
    std::size_t primary_card_count = 0;
    std::array<Rect, 3> sliders {};
    std::array<Rect, 2> secondary_actions {};
    std::array<Rect, 3> system_actions {};
    Rect network_toggle;
    std::array<Rect, 4> network_rows {};
    Rect network_settings;
    AuxiliaryTile fourth_primary_tile = AuxiliaryTile::OnScreenKeyboard;
};

[[nodiscard]] QuickSettingsLayout make_layout(Extent display,
                                                const QuickSettingsModel& model);
[[nodiscard]] bool valid_layout(const QuickSettingsLayout& layout, Extent display);

}  // namespace tdvp::quick_settings
