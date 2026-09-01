#pragma once

namespace tdvp::quick_settings {

enum class DrawerSettleTarget {
    Collapsed,
    Expanded,
};

// Clamp a panel's revealed height to its visual extent. During direct
// manipulation this is the single source of truth for what the user sees.
[[nodiscard]] float clamp_drawer_revealed(float revealed_px, float extent_px);

// Map the finger displacement from a gesture's starting point to a revealed
// height. Positive displacement opens the drawer; negative displacement closes
// it. The same operation makes a reversed finger movement immediately reverse
// the drawer without a special cancel path.
[[nodiscard]] float drawer_revealed_from_drag(float revealed_on_down_px,
                                               float vertical_displacement_px,
                                               float extent_px);

// Match the useful mobile rule: a decisive fling chooses its direction, while
// a slow release settles to the nearest stable state.
[[nodiscard]] DrawerSettleTarget drawer_settle_target(float revealed_px, float extent_px,
                                                       float velocity_px_per_second,
                                                       float fling_threshold_px_per_second = 700.0F);

// Keep the post-release animation short for a near target and bounded even for
// a slow gesture. This is used only after the finger has left the screen.
[[nodiscard]] int drawer_settle_duration_ms(float revealed_px, float extent_px,
                                             DrawerSettleTarget target,
                                             float velocity_px_per_second);

// A settling frame is interpolated from the exact hand-controlled position,
// avoiding a discontinuity at ACTION_UP / wl_touch.up.
[[nodiscard]] float drawer_settle_revealed(float revealed_on_release_px, float extent_px,
                                            DrawerSettleTarget target, float progress);

}  // namespace tdvp::quick_settings
