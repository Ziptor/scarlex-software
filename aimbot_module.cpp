#include "runtime_log.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "aimbot_module.h"

#include "OverlayD3D11.h"
#include "VisibilityClient.h"
#include "core/math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>

namespace aimbot {
namespace {

using game_entities::Entity;
using game_entities::GameEntities;
constexpr auto clip_depth = core::ClipDepthRange::zero_to_one;

struct Target {
    std::uint64_t address;
    core::Vec2 screen;
    float radial_distance;
    float world_distance;
    visibility::Result visibility;
};

struct SearchStats {
    std::size_t valid{}, in_range{}, too_far{}, behind{}, projected{}, in_fov{};
    std::size_t missing_zone{};
    std::size_t blocked_points{}, unknown_points{};

    const char* reason() const noexcept {
        if (unknown_points) return "no confirmed visible target; visibility data unavailable or stale";
        if (blocked_points) return "all in-FOV points are occluded";
        if (valid == 0) return "no valid remote entities";
        if (valid == too_far) return "target too far";
        if (in_range == 0) return "no targets within distance limit";
        if (in_range == missing_zone) return "selected limb positions unavailable";
        if (in_range == behind) return "all targets behind camera";
        if (projected == 0) return "no targets project to viewport";
        return "no targets in FOV";
    }
};

core::Vec3 as_vector(game_entities::Vec3 v) noexcept { return {v.x, v.y, v.z}; }

std::optional<core::Vec3> aim_point(const Entity& entity, const AimbotSettings& settings,
                                   bool right = false, bool apply_prediction = true) noexcept {
    auto point = as_vector(entity.head_position);
    if (settings.target_bone == TargetZone::hands || settings.target_bone == TargetZone::legs) {
        if (!entity.limbs_valid) return std::nullopt;
        using game_entities::LimbJoint;
        const auto joint = settings.target_bone == TargetZone::hands
            ? (right ? LimbJoint::right_hand : LimbJoint::left_hand)
            : (right ? LimbJoint::right_knee : LimbJoint::left_knee);
        point = as_vector(entity.limb_joints[static_cast<std::size_t>(joint)]);
    } else if (settings.target_bone == TargetZone::body) {
        if (entity.skeleton_valid) {
            point = as_vector(entity.bones[static_cast<std::size_t>(game_entities::SkeletonBone::spine3)]);
        } else {
            const auto& feet = entity.position;
            point = {std::lerp(feet.x, point.x, 0.70f), std::lerp(feet.y, point.y, 0.70f),
                     std::lerp(feet.z, point.z, 0.70f)};
        }
    } else {
        point.y -= settings.head_offset;
    }
    if (settings.predict && apply_prediction) {
        const auto velocity = as_vector(entity.velocity);
        if (core::math::finite(velocity)) {
            const float seconds = entity.distance / settings.bullet_speed;
            point.x += velocity.x * seconds;
            point.y += velocity.y * seconds;
            point.z += velocity.z * seconds;
        }
    }
    return point;
}

std::optional<Target> select_target(std::span<const Entity> entities, std::uint64_t local,
                                    const AimbotSettings& settings,
                                    const core::Mat4& projection, int width, int height,
                                    SearchStats* stats = nullptr,
                                    visibility::VisibilityClient* visibility_client = nullptr,
                                    core::Vec3 eye = {}) noexcept {
    const core::Vec2 center{static_cast<float>(width) * 0.5f, static_cast<float>(height) * 0.5f};
    std::optional<Target> best;
    for (const auto& entity : entities) {
        if (!entity.address || entity.address == local || !entity.valid) {
            continue;
        }
        if (settings.target_role != game_entities::CharacterRole::unknown &&
            entity.role != settings.target_role) {
            continue;
        }
        if (stats) ++stats->valid;
        if (!std::isfinite(entity.distance) || entity.distance < 0.0f) {
            continue;
        }
        if (entity.distance > settings.max_distance) {
            if (stats) ++stats->too_far;
            continue;
        }
        if (stats) ++stats->in_range;
        const bool paired = settings.target_bone >= TargetZone::hands;
        std::optional<Target> candidate;
        bool has_point{}, all_behind = true, projected{}, in_fov{};
        for (int side = 0; side < (paired ? 2 : 1); ++side) {
            const auto point = aim_point(entity, settings, side != 0);
            if (!point || !core::math::finite(*point)) continue;
            has_point = true;
            if (const auto screen = core::math::world_to_screen(*point, projection, width, height, clip_depth)) {
                projected = true;
                const float distance = std::hypot(screen->x - center.x, screen->y - center.y);
                if (!(distance < settings.fov_radius)) continue;
                in_fov = true;
                auto sight = visibility::Result::unknown;
                if (settings.visible_only) {
                    const auto current = aim_point(entity, settings, side != 0, false);
                    if (visibility_client && current && core::math::finite(*current)) {
                        sight = visibility_client->query({entity.address, settings.target_bone, side, eye, *current});
                    }
                    if (sight != visibility::Result::visible) {
                        if (stats && sight == visibility::Result::blocked) ++stats->blocked_points;
                        if (stats && sight == visibility::Result::unknown) ++stats->unknown_points;
                        continue;
                    }
                }
                if (!candidate || distance < candidate->radial_distance) {
                    candidate = Target{entity.address, *screen, distance, entity.distance, sight};
                }
            } else if (stats && core::math::transform(projection, *point).w > 0.0f) {
                all_behind = false;
            }
        }
        if (!projected) {
            if (stats && !has_point) ++stats->missing_zone;
            else if (stats && all_behind) ++stats->behind;
            continue;
        }
        if (stats) ++stats->projected;
        if (stats && in_fov) ++stats->in_fov;
        if (!candidate) continue;
        const float distance = candidate->radial_distance;
        if (!best || distance < best->radial_distance ||
            (distance == best->radial_distance && entity.address < best->address)) {
            best = candidate;
        }
    }
    return best;
}

}  // namespace

AimbotModule::AimbotModule(AimbotSettings settings) noexcept {
    set_settings(settings);
}

void AimbotModule::set_settings(AimbotSettings settings) noexcept {
    constexpr AimbotSettings defaults;
    settings.fov_radius = std::isfinite(settings.fov_radius)
        ? std::clamp(settings.fov_radius, 0.0f, 16384.0f) : defaults.fov_radius;
    settings.smooth = std::isfinite(settings.smooth)
        ? std::clamp(settings.smooth, 0.1f, 1.0f) : defaults.smooth;
    settings.max_distance = std::isfinite(settings.max_distance) && settings.max_distance > 0.0f
        ? settings.max_distance : defaults.max_distance;
    settings.bullet_speed = std::isfinite(settings.bullet_speed) && settings.bullet_speed >= 1.0f
        ? settings.bullet_speed : defaults.bullet_speed;
    if (settings.target_bone < TargetZone::head || settings.target_bone > TargetZone::legs) {
        settings.target_bone = TargetZone::head;
    }
    settings.head_offset = std::isfinite(settings.head_offset) && settings.head_offset >= 0.0f
        ? settings.head_offset : defaults.head_offset;
    settings_ = settings;
    reset();
    logged_stage_.clear();
}

void AimbotModule::reset() noexcept {
    target_address_ = 0;
    target_screen_.reset();
    remainder_ = {};
    last_input_error_.reset();
    reported_input_error_.reset();
}

bool AimbotModule::trace_stage(std::string_view stage, bool key_pressed) {
    if constexpr (!scarlex::build::diagnostics) return false;
    const auto now = Clock::now();
    if (!settings_.trace_every_update && stage == logged_stage_ &&
        key_pressed == logged_key_pressed_ && now < trace_after_) {
        return false;
    }
    logged_stage_ = stage;
    logged_key_pressed_ = key_pressed;
    trace_after_ = now + std::chrono::seconds(1);
    scarlex::diagnostic_printf("[AIM] Module called, enabled=%d, key_pressed=%d\n", settings_.enabled, key_pressed);
    return true;
}

void AimbotModule::on_update(const GameEntities& snapshot, int width, int height,
                             bool input_allowed, bool new_snapshot, const char* input_block_reason,
                             visibility::VisibilityClient* visibility_client) {
    last_input_error_.reset();
    const bool valid_key = settings_.aim_key > 0 && settings_.aim_key < 0xff;
    // The default aim key is VK_RBUTTON. Only the high bit represents the held state.
    const bool key_pressed = valid_key && GetAsyncKeyState(settings_.aim_key) < 0;
    const char* reason = nullptr;
    if (!settings_.enabled) {
        reason = "disabled";
    } else if (!valid_key) {
        reason = "invalid aim key";
    } else if (!key_pressed) {
        reason = "aim key is released";
    } else if (!input_allowed) {
        reason = input_block_reason ? input_block_reason : "input disallowed";
    } else if (width <= 0 || height <= 0) {
        reason = "invalid viewport";
    }
    if (reason) {
        if (trace_stage(reason, key_pressed)) {
            scarlex::diagnostic_printf("[AIM] Skipped: %s\n", reason);
            std::fflush(stdout);
        }
        reset();
        return;
    }
    if (!new_snapshot) {
        return;
    }
    const bool trace = trace_stage("search", key_pressed);
    const auto& camera = snapshot.camera();
    const float fov = camera.fov > 0.0f && camera.fov < 179.0f ? camera.fov : 90.0f;
    const auto projection = core::math::view_projection(
        {as_vector(camera.position), as_vector(camera.forward), as_vector(camera.right), as_vector(camera.up)},
        {fov, camera.near_plane, camera.far_plane, clip_depth},
        static_cast<float>(width) / static_cast<float>(height));
    if (!projection) {
        if (trace) {
            std::puts("[AIM] Skipped: invalid camera projection");
            std::fflush(stdout);
        }
        reset();
        return;
    }
    SearchStats stats;
    const bool query_visibility = settings_.visible_only && visibility_client;
    if (query_visibility) visibility_client->begin_frame();
    const auto target = select_target(snapshot.entities(), snapshot.local_player_address(),
                                      settings_, *projection, width, height, trace ? &stats : nullptr,
                                      visibility_client, as_vector(camera.position));
    if (query_visibility) visibility_client->end_frame();
    if (trace) {
        scarlex::diagnostic_printf("[AIM] Found %zu valid targets in FOV\n", stats.in_fov);
    }
    if (!target) {
        if (trace) {
            if (settings_.visible_only && stats.in_fov) {
                const auto result = stats.unknown_points ? visibility::Result::unknown : visibility::Result::blocked;
                scarlex::diagnostic_printf("[AIM] target visible=%d (blocked=%zu, unknown=%zu)\n",
                            static_cast<int>(result), stats.blocked_points, stats.unknown_points);
            }
            scarlex::diagnostic_printf("[AIM] No target: %s (valid=%zu, too_far=%zu, in_range=%zu, behind=%zu, projected=%zu)\n",
                        stats.reason(), stats.valid, stats.too_far, stats.in_range, stats.behind, stats.projected);
            std::fflush(stdout);
        }
        reset();
        return;
    }
    if (target_address_ != target->address) {
        remainder_ = {};
    }
    target_address_ = target->address;
    target_screen_ = target->screen;
    const float center_x = static_cast<float>(width) * 0.5f, center_y = static_cast<float>(height) * 0.5f;
    const float error_x = target->screen.x - center_x, error_y = target->screen.y - center_y;
    if (trace) {
        if (settings_.visible_only) {
            scarlex::diagnostic_printf("[AIM] target visible=%d\n", static_cast<int>(target->visibility));
        }
        scarlex::diagnostic_printf("[AIM] Target screen=(%.1f, %.1f) center=(%.1f, %.1f) delta=(%.1f, %.1f) distance=%.1f m\n",
                    target->screen.x, target->screen.y, center_x, center_y, error_x, error_y, target->world_distance);
    }
    const float step_x = error_x == 0.0f ? 0.0f : error_x * settings_.smooth + remainder_.x;
    const float step_y = error_y == 0.0f ? 0.0f : error_y * settings_.smooth + remainder_.y;
    const LONG dx = std::lround(step_x), dy = std::lround(step_y);
    remainder_ = {step_x - static_cast<float>(dx), step_y - static_cast<float>(dy)};
    if (dx == 0 && dy == 0) {
        if (trace) {
            std::puts("[AIM] Skipped: rounded mouse delta is zero");
            std::fflush(stdout);
        }
        return;
    }

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    if (GetAsyncKeyState(settings_.aim_key) >= 0) {
        if (trace_stage("aim key released before SendInput", false)) {
            std::puts("[AIM] Skipped: aim key released before SendInput");
            std::fflush(stdout);
        }
        reset();
        return;
    }
    SetLastError(ERROR_SUCCESS);
    const bool sent = SendInput(1, &input, static_cast<int>(sizeof(input))) == 1;
    const DWORD error = sent ? ERROR_SUCCESS : GetLastError();
    if (trace) {
        scarlex::diagnostic_printf("[AIM] Sending mouse move: dx=%.1f, dy=%.1f\n", static_cast<double>(dx), static_cast<double>(dy));
    }
    if (!sent) {
        last_input_error_ = error;
        if (trace || reported_input_error_ != last_input_error_) {
            scarlex::diagnostic_printf("[AIM] SendInput failed, GetLastError=%lu\n", static_cast<unsigned long>(*last_input_error_));
            std::fflush(stdout);
        }
        reported_input_error_ = last_input_error_;
        remainder_ = {};
    } else {
        reported_input_error_.reset();
    }
    if (trace) {
        std::fflush(stdout);
    }
}

void AimbotModule::on_render(const OverlayD3D11& overlay) const {
    if (!settings_.enabled || settings_.fov_radius <= 0.0f || overlay.width() <= 0 || overlay.height() <= 0) {
        return;
    }
    const float x = static_cast<float>(overlay.width()) * 0.5f;
    const float y = static_cast<float>(overlay.height()) * 0.5f;
    if (settings_.show_fov) {
        overlay.draw_circle(x, y, settings_.fov_radius, settings_.indicator_color);
    }
    if (settings_.show_target_line && target_screen_ && GetAsyncKeyState(settings_.aim_key) < 0) {
        overlay.draw_line(x, y, target_screen_->x, target_screen_->y, settings_.indicator_color);
    }
}

}  // namespace aimbot

