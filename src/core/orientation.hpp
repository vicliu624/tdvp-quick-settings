#pragma once

#include "core/layout.hpp"

namespace tdvp::quick_settings {

// Values intentionally follow wl_output_transform without making the core
// model depend on the Wayland client headers.
enum class SurfaceTransform : int {
    Normal = 0,
    Rotate90 = 1,
    Rotate180 = 2,
    Rotate270 = 3,
};

struct Point {
    int x = 0;
    int y = 0;
};

[[nodiscard]] bool is_quarter_turn(SurfaceTransform transform);
[[nodiscard]] SurfaceTransform inverse_transform(SurfaceTransform transform);
[[nodiscard]] Extent buffer_extent_for_surface(Extent surface, SurfaceTransform transform);
[[nodiscard]] Point surface_to_buffer(Point surface_point, Extent buffer,
                                      SurfaceTransform transform);

}  // namespace tdvp::quick_settings
