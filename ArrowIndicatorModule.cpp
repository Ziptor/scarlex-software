#include "ArrowIndicatorModule.h"

#include "OverlayD3D11.h"
#include "game_entities.h"
#include "core/math.h"
#include "imgui.h"
#include "TextureManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace {

core::Vec3 arrow_vector(game_entities::Vec3 v) noexcept { return {v.x, v.y, v.z}; }

std::optional<std::array<ImVec2, 3>> arrow_triangle(
    const game_entities::Entity& target, const game_entities::Camera& camera,
    const ArrowIndicatorSettings& settings, int width, int height,
    const std::optional<core::Mat4>& matrix) {
    const auto world = arrow_vector(target.position);
    if (width <= 0 || height <= 0 || !target.valid || !core::math::finite(world) ||
        !std::isfinite(target.distance) || target.distance < 0.0f || target.distance > settings.max_distance) {
        return std::nullopt;
    }
    const core::Vec2 center{static_cast<float>(width) * 0.5f, static_cast<float>(height) * 0.5f};
    const auto screen = matrix ? core::math::world_to_screen(world, *matrix, width, height,
                                core::ClipDepthRange::zero_to_one) : std::nullopt;
    if (settings.show_only_offscreen && screen) return std::nullopt;
    core::Vec2 direction{};
    if (screen) {
        direction = {screen->x - center.x, screen->y - center.y};
    } else {
        const auto camera_position = arrow_vector(camera.position);
        if (!core::math::finite(camera_position)) return std::nullopt;
        const core::Vec3 relative{world.x - camera_position.x, world.y - camera_position.y,
                                  world.z - camera_position.z};
        const auto right = arrow_vector(camera.right), forward = arrow_vector(camera.forward);
        if (!core::math::finite(right) || !core::math::finite(forward)) return std::nullopt;
        direction = {relative.x * right.x + relative.y * right.y + relative.z * right.z,
                     -(relative.x * forward.x + relative.y * forward.y + relative.z * forward.z)};
    }
    const float length = std::hypot(direction.x, direction.y);
    if (!std::isfinite(length) || length < 1.0e-4f) return std::nullopt;
    direction.x /= length;
    direction.y /= length;
    const float available = (std::min)(center.x, center.y) - settings.arrow_size - 4.0f;
    if (available < 32.0f) return std::nullopt;
    const float radius = (std::min)(settings.arrow_radius, available);
    const ImVec2 anchor{center.x + direction.x * radius, center.y + direction.y * radius};
    const float size = settings.arrow_size;
    const ImVec2 tip{anchor.x + direction.x * size, anchor.y + direction.y * size};
    const ImVec2 base{anchor.x - direction.x * size * 0.6f, anchor.y - direction.y * size * 0.6f};
    return std::array<ImVec2, 3>{tip,
        ImVec2{base.x - direction.y * size * 0.55f, base.y + direction.x * size * 0.55f},
        ImVec2{base.x + direction.y * size * 0.55f, base.y - direction.x * size * 0.55f}};
}

ImU32 arrow_color(game_entities::CharacterRole role) noexcept {
    using game_entities::CharacterRole;
    switch (role) {
    case CharacterRole::cop: return 0xFFFF702Fu;
    case CharacterRole::robber: return 0xFF3535FFu;
    default: return 0xFFFFFFFFu;
    }
}

}  // namespace

ArrowIndicatorModule::ArrowIndicatorModule(ArrowIndicatorSettings settings) noexcept : settings_(settings) {
    const auto bounded = [](float value, float minimum, float maximum, float fallback) {
        return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
    };
    settings_.arrow_radius = bounded(settings_.arrow_radius, 32.0f, 1000.0f, 80.0f);
    settings_.arrow_size = bounded(settings_.arrow_size, 4.0f, 24.0f, 10.0f);
    settings_.max_distance = bounded(settings_.max_distance, 1.0f, 10000.0f, 200.0f);
}

void ArrowIndicatorModule::on_render(const game_entities::GameEntities& snapshot,
                                    const OverlayD3D11& overlay, bool menu_open, float weapon_icon_size) const {
    if (!settings_.enabled || menu_open) return;
    const int width = overlay.width(), height = overlay.height();
    if (width <= 0 || height <= 0) return;
    const auto& camera = snapshot.camera();
    const auto matrix = core::math::view_projection(
        {arrow_vector(camera.position), arrow_vector(camera.forward),
         arrow_vector(camera.right), arrow_vector(camera.up)},
        {camera.fov > 0.0f && camera.fov < 179.0f ? camera.fov : 90.0f, camera.near_plane,
         camera.far_plane, core::ClipDepthRange::zero_to_one},
        static_cast<float>(width) / static_cast<float>(height));
    auto* draw = ImGui::GetBackgroundDrawList();
    for (const auto& target : snapshot.entities()) {
        if (!target.address || target.address == snapshot.local_player_address()) continue;
        const auto triangle = arrow_triangle(target, camera, settings_, width, height, matrix);
        if (!triangle) continue;
        draw->AddTriangleFilled((*triangle)[0], (*triangle)[1], (*triangle)[2], arrow_color(target.role));
        draw->AddTriangle((*triangle)[0], (*triangle)[1], (*triangle)[2], 0xFF181818u, 1.0f);
        if (settings_.arrow_show_weapon_icon && target.has_weapon) {
            auto* textures=overlay.texture_manager();
            const auto icon=textures ? textures->WeaponIcon(target.weapon_id) : ImTextureID{};
            if (icon) {
                const auto size=textures->IconSize(icon,weapon_icon_size);
                const auto& tip=(*triangle)[0];
                const ImVec2 base{((*triangle)[1].x+(*triangle)[2].x)*0.5f,
                                  ((*triangle)[1].y+(*triangle)[2].y)*0.5f};
                const float length=std::hypot(tip.x-base.x,tip.y-base.y);
                const ImVec2 direction{(tip.x-base.x)/length,(tip.y-base.y)/length};
                const float gap=4.0f+(std::abs(direction.x)*size.x+std::abs(direction.y)*size.y)*0.5f;
                const float x=std::clamp(tip.x+direction.x*gap,size.x*0.5f+2.0f,
                                         static_cast<float>(width)-size.x*0.5f-2.0f);
                const float y=std::clamp(tip.y+direction.y*gap,size.y*0.5f+2.0f,
                                         static_cast<float>(height)-size.y*0.5f-2.0f);
                draw->AddImage(icon,{x-size.x*0.5f,y-size.y*0.5f},{x+size.x*0.5f,y+size.y*0.5f});
            }
        }
    }
}
