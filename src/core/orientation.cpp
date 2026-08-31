#include "core/orientation.hpp"

#include <algorithm>

namespace tdvp::quick_settings {

bool is_quarter_turn(SurfaceTransform transform)
{
    return transform == SurfaceTransform::Rotate90 || transform == SurfaceTransform::Rotate270;
}

SurfaceTransform inverse_transform(SurfaceTransform transform)
{
    switch (transform) {
    case SurfaceTransform::Rotate90: return SurfaceTransform::Rotate270;
    case SurfaceTransform::Rotate270: return SurfaceTransform::Rotate90;
    case SurfaceTransform::Rotate180: return SurfaceTransform::Rotate180;
    case SurfaceTransform::Normal: return SurfaceTransform::Normal;
    }
    return SurfaceTransform::Normal;
}

Extent buffer_extent_for_surface(Extent surface, SurfaceTransform transform)
{
    if (is_quarter_turn(transform))
        return Extent {surface.height, surface.width};
    return surface;
}

Point surface_to_buffer(Point surface_point, Extent buffer, SurfaceTransform transform)
{
    Point mapped {};
    switch (transform) {
    case SurfaceTransform::Rotate90:
        mapped = Point {surface_point.y, buffer.height - 1 - surface_point.x};
        break;
    case SurfaceTransform::Rotate180:
        mapped = Point {buffer.width - 1 - surface_point.x, buffer.height - 1 - surface_point.y};
        break;
    case SurfaceTransform::Rotate270:
        mapped = Point {buffer.width - 1 - surface_point.y, surface_point.x};
        break;
    case SurfaceTransform::Normal:
        mapped = surface_point;
        break;
    }
    mapped.x = std::max(0, std::min(buffer.width - 1, mapped.x));
    mapped.y = std::max(0, std::min(buffer.height - 1, mapped.y));
    return mapped;
}

}  // namespace tdvp::quick_settings
