#include "RadarModule.h"

#include "OverlayD3D11.h"
#include "core/types.h"
#include "game_entities.h"

#include <algorithm>
#include <cmath>

namespace {

using game_entities::Vec3;

std::uint32_t player_color(game_entities::CharacterRole role, const RadarSettings& settings) noexcept {
    using game_entities::CharacterRole;
    switch (role) {
    case CharacterRole::cop: return settings.cop_color;
    case CharacterRole::robber: return settings.robber_color;
    default: return settings.neutral_color;
    }
}

void draw_marker(const OverlayD3D11& overlay, core::Vec2 point, float half_size, std::uint32_t color) {
    overlay.draw_rect(point.x - half_size, point.y - half_size,
                      point.x + half_size, point.y + half_size, color, half_size);
}

}  // namespace

RadarModule::RadarModule(RadarSettings settings) noexcept : settings_(settings) {
    constexpr RadarSettings defaults;
    if (!std::isfinite(settings_.radar_size) || settings_.radar_size < 12.0f) {
        settings_.radar_size = defaults.radar_size;
    }
    if (!std::isfinite(settings_.radar_scale) || settings_.radar_scale <= 0.0f) {
        settings_.radar_scale = defaults.radar_scale;
    }
    settings_.position_x = std::isfinite(settings_.position_x)
        ? std::clamp(settings_.position_x, 0.0f, 1.0f) : defaults.position_x;
    settings_.position_y = std::isfinite(settings_.position_y)
        ? std::clamp(settings_.position_y, 0.0f, 1.0f) : defaults.position_y;
}

std::optional<RadarPlacement> RadarModule::placement(RECT work, RECT drawable) const noexcept {
    if (!settings_.enabled || (!settings_.show_players && !settings_.show_loot)) {
        return std::nullopt;
    }
    constexpr float margin = 16.0f;
    if (drawable.right <= drawable.left || drawable.bottom <= drawable.top) return std::nullopt;
    RECT clipped_work{};
    work = IntersectRect(&clipped_work, &work, &drawable) ? clipped_work : drawable;
    const bool full_area = settings_.custom_position && settings_.position_in_client;
    const RECT area = full_area ? drawable : work;
    const float width = static_cast<float>(area.right - area.left);
    const float height = static_cast<float>(area.bottom - area.top);
    const float radius = (std::min)(settings_.radar_size,
        (std::min)(width, height) * 0.5f - (full_area ? 0.0f : margin));
    if (radius < 12.0f) {
        return std::nullopt;
    }
    const float offset = margin + radius;
    const bool right = (settings_.corner & 1) != 0;
    const bool bottom = (settings_.corner & 2) != 0;
    const float left = static_cast<float>(drawable.left);
    const float top = static_cast<float>(drawable.top);
    const float right_edge = static_cast<float>(drawable.right - 1);
    const float bottom_edge = static_cast<float>(drawable.bottom - 1);
    const float x = settings_.custom_position ? std::clamp(settings_.position_x, 0.0f, 1.0f) : right ? 1.0f : 0.0f;
    const float y = settings_.custom_position ? std::clamp(settings_.position_y, 0.0f, 1.0f) : bottom ? 1.0f : 0.0f;
    const float center_x = full_area ? std::lerp(left, right_edge, x)
        : std::lerp(static_cast<float>(work.left) + offset, static_cast<float>(work.right) - offset, x);
    const float center_y = full_area ? std::lerp(top, bottom_edge, y)
        : std::lerp(static_cast<float>(work.top) + offset, static_cast<float>(work.bottom) - offset, y);
    return RadarPlacement{center_x, center_y, radius, left, top, right_edge, bottom_edge};
}

void RadarModule::on_render(const game_entities::GameEntities& snapshot, const OverlayD3D11& overlay) const {
    const auto area = placement(overlay.visible_client_rect(), overlay.desktop_client_rect());
    if (!area) return;
    const core::Vec2 center{area->x, area->y};
    const float radius = area->radius;
    overlay.fill_circle(center.x, center.y, radius, settings_.background);
    overlay.draw_circle(center.x, center.y, radius, settings_.border);
    if (settings_.grid) {
        overlay.draw_circle(center.x, center.y, radius * 0.5f, 0x55FFFFFFu);
        overlay.draw_line(center.x, center.y - radius, center.x, center.y + radius, 0x45FFFFFFu);
        overlay.draw_line(center.x - radius, center.y, center.x + radius, center.y, 0x45FFFFFFu);
    }

    const auto& players = snapshot.entities();
    const auto& origin = snapshot.get_local_player_position();
    if (!origin || !std::isfinite(origin->x) || !std::isfinite(origin->z)) {
        return;
    }
    const auto& camera = snapshot.camera();
    float forward_x = settings_.camera_relative ? camera.forward.x : 0.0f;
    float forward_z = settings_.camera_relative ? camera.forward.z : 1.0f;
    float heading_length = std::hypot(forward_x, forward_z);
    if (!std::isfinite(heading_length) || heading_length < 1.0e-4f) {
        // At vertical pitch the camera's horizontal right vector still defines yaw.
        forward_x = -camera.right.z;
        forward_z = camera.right.x;
        heading_length = std::hypot(forward_x, forward_z);
        if (!std::isfinite(heading_length) || heading_length < 1.0e-4f) {
            return;
        }
    }
    const float yaw = std::atan2(forward_x, forward_z);
    const float right_x = std::cos(yaw), right_z = -std::sin(yaw);
    const float ahead_x = -right_z, ahead_z = right_x;
    const auto plot = [&](Vec3 position, float half_size, std::uint32_t color) {
        const float x = position.x - origin->x;
        const float z = position.z - origin->z;
        // Project onto camera right and forward. Screen Y increases downwards.
        const float dx = (x * right_x + z * right_z) / settings_.radar_scale;
        const float dy = -(x * ahead_x + z * ahead_z) / settings_.radar_scale;
        const float distance = std::hypot(dx, dy);
        if (std::isfinite(distance) && distance <= radius - half_size * 2.0f) {
            draw_marker(overlay, {center.x + dx, center.y + dy}, half_size, color);
        }
    };
    if (settings_.show_loot) {
        for (const auto& item : snapshot.loot_items()) {
            plot(item.position, settings_.marker_size * (2.0f / 3.0f), settings_.loot_color);
        }
    }
    if (settings_.show_players) {
        for (const auto& player : players) {
            if (player.address && player.address != snapshot.local_player_address() && std::isfinite(player.distance)) {
                plot(player.position, settings_.marker_size, player_color(player.role, settings_));
            }
        }
    }
    draw_marker(overlay, center, settings_.marker_size, settings_.local_color);
}
