#include "core/drawer.hpp"

#include <algorithm>
#include <cmath>

namespace tdvp::quick_settings {
namespace {

constexpr int kMinimumSettleDurationMs = 90;
constexpr int kMaximumSettleDurationMs = 220;
constexpr float kFallbackSettleVelocityPxPerSecond = 1800.0F;

float target_revealed(float extent_px, DrawerSettleTarget target)
{
    return target == DrawerSettleTarget::Expanded ? extent_px : 0.0F;
}

}  // namespace

float clamp_drawer_revealed(float revealed_px, float extent_px)
{
    return std::max(0.0F, std::min(revealed_px, std::max(0.0F, extent_px)));
}

float drawer_revealed_from_drag(float revealed_on_down_px, float vertical_displacement_px,
                                float extent_px)
{
    return clamp_drawer_revealed(revealed_on_down_px + vertical_displacement_px, extent_px);
}

DrawerSettleTarget drawer_settle_target(float revealed_px, float extent_px,
                                        float velocity_px_per_second,
                                        float fling_threshold_px_per_second)
{
    if (std::abs(velocity_px_per_second) >= std::max(0.0F, fling_threshold_px_per_second)) {
        return velocity_px_per_second > 0.0F ? DrawerSettleTarget::Expanded
                                             : DrawerSettleTarget::Collapsed;
    }
    return clamp_drawer_revealed(revealed_px, extent_px) >= std::max(0.0F, extent_px) * 0.5F
               ? DrawerSettleTarget::Expanded
               : DrawerSettleTarget::Collapsed;
}

int drawer_settle_duration_ms(float revealed_px, float extent_px, DrawerSettleTarget target,
                              float velocity_px_per_second)
{
    const float safe_extent = std::max(1.0F, extent_px);
    const float distance = std::abs(target_revealed(safe_extent, target) -
                                    clamp_drawer_revealed(revealed_px, safe_extent));
    const float velocity = std::max(std::abs(velocity_px_per_second),
                                    kFallbackSettleVelocityPxPerSecond);
    const float velocity_duration = distance * 1000.0F / velocity;
    const float distance_duration = 100.0F + 120.0F * (distance / safe_extent);
    const float requested = std::max(velocity_duration, distance_duration);
    return static_cast<int>(std::round(std::max(static_cast<float>(kMinimumSettleDurationMs),
                                                std::min(requested,
                                                         static_cast<float>(kMaximumSettleDurationMs)))));
}

float drawer_settle_revealed(float revealed_on_release_px, float extent_px,
                             DrawerSettleTarget target, float progress)
{
    const float safe_extent = std::max(0.0F, extent_px);
    const float start = clamp_drawer_revealed(revealed_on_release_px, safe_extent);
    const float end = target_revealed(safe_extent, target);
    const float t = std::max(0.0F, std::min(progress, 1.0F));
    const float ease_out = 1.0F - std::pow(1.0F - t, 3.0F);
    return start + (end - start) * ease_out;
}

}  // namespace tdvp::quick_settings
