#include "runtime_log.h"
#include "ESPModule.h"

#include "OverlayD3D11.h"
#include "core/math.h"
#include "game_entities.h"
#include "game_data.h"
#include "TextureManager.h"
#include "EspLayout.h"
#include "imgui.h"
#include "ItemPresentation.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <optional>

namespace {

constexpr float fallback_fov = 90.0f;
std::uint32_t role_color(game_entities::CharacterRole role, const ESPSettings& settings) {
    using game_entities::CharacterRole;
    if (!settings.role_colors) return settings.box_color;
    switch (role) {
    case CharacterRole::cop: return settings.cop_color;
    case CharacterRole::robber: return settings.robber_color;
    default: return settings.box_color;
    }
}
constexpr std::uint32_t outline = 0xCC161413;
constexpr auto depth = core::ClipDepthRange::zero_to_one;
constexpr std::array<const char*, game_entities::skeleton_bone_count> bone_names{
    "Root", "Legs", "Spine2", "Spine3", "Neck", "Head1", "Head2"
};
constexpr std::array<const char*, game_entities::limb_joint_count> limb_names{
    "Pelvis", "LeftShoulder", "LeftElbow", "LeftHand", "RightShoulder", "RightElbow", "RightHand",
    "LeftHip", "LeftKnee", "LeftFoot", "RightHip", "RightKnee", "RightFoot"
};

core::Vec3 as_vector(const game_entities::Vec3& v) noexcept {
    return {v.x, v.y, v.z};
}

void log_point(const char* name, core::Vec3 world, const core::Mat4* matrix,
               int width, int height, bool valid, bool bone = false) {
    scarlex::diagnostic_printf("[ESP]   %s%s world=(%.3f,%.3f,%.3f)", bone ? "bone=" : "", name,
                world.x, world.y, world.z);
    if (!valid || !matrix || !core::math::finite(world)) {
        const char* reason = !valid ? "invalid-head" : !matrix ? "invalid-camera" : "invalid-world";
        scarlex::diagnostic_printf(" screen_x=NA screen_y=NA status=%s\n", reason);
        return;
    }

    // Log the unclipped pixel coordinates. The renderer applies frustum clipping.
    const auto clip = core::math::transform(*matrix, world);
    if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
        !std::isfinite(clip.z) || !std::isfinite(clip.w)) {
        scarlex::diagnostic_printf(" screen_x=NA screen_y=NA status=invalid-clip\n");
        return;
    }
    if (clip.w <= 1.0e-5f) {
        scarlex::diagnostic_printf(" screen_x=NA screen_y=NA clip_w=%.4f %s%s\n", clip.w,
                    bone ? "status=clipped reason=" : "status=",
                    clip.w <= 0.0f ? "behind-camera" : "near-zero-depth");
        return;
    }

    const double x = (1.0 + static_cast<double>(clip.x) / clip.w) * 0.5 * width;
    const double y = (1.0 - static_cast<double>(clip.y) / clip.w) * 0.5 * height;
    const char* clipped_by = nullptr;
    if (clip.z < 0.0f || clip.z > clip.w) {
        clipped_by = "outside-depth";
    } else if (std::abs(clip.x) > clip.w || std::abs(clip.y) > clip.w) {
        clipped_by = "outside-viewport";
    }
    scarlex::diagnostic_printf(" screen_x=%.2f screen_y=%.2f clip_w=%.4f %s%s\n", x, y, clip.w,
                bone && clipped_by ? "status=clipped reason=" : "status=",
                clipped_by ? clipped_by : "on-screen");
}

std::size_t draw_skeleton(const game_entities::Entity& entity, const core::Mat4& matrix,
                          int width, int height, const OverlayD3D11& overlay,
                          const ESPSettings& settings, bool report) {
    using game_entities::SkeletonBone;
    constexpr auto legs = static_cast<std::size_t>(SkeletonBone::legs);
    constexpr auto neck = static_cast<std::size_t>(SkeletonBone::neck);
    constexpr auto root = static_cast<std::size_t>(SkeletonBone::root);
    constexpr auto spine = static_cast<std::size_t>(SkeletonBone::spine3);
    constexpr auto head_tip = static_cast<std::size_t>(SkeletonBone::head2);
    std::array<std::optional<core::Vec2>, game_entities::skeleton_bone_count> points{};
    const auto address = static_cast<unsigned long long>(entity.address);
    if (report) {
        scarlex::diagnostic_printf("[ESP] projecting skeleton for entity=0x%llX bones=%zu\n", address, points.size());
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        auto world = as_vector(entity.bones[i]);
        const auto parent = entity.bone_parents[i];
        if (i == head_tip && parent >= 0 && static_cast<std::size_t>(parent) < points.size()) {
            const auto anchor = as_vector(entity.bones[static_cast<std::size_t>(parent)]);
            world = {std::lerp(anchor.x, world.x, settings.head_length),
                     std::lerp(anchor.y, world.y, settings.head_length),
                     std::lerp(anchor.z, world.z, settings.head_length)};
        }
        points[i] = core::math::world_to_screen(world, matrix, width, height, depth);
        if (report) {
            log_point(bone_names[i], world, &matrix, width, height, true, true);
        }
    }
    std::size_t drawn{};
    const auto link = [&](const std::optional<core::Vec2>& start, const std::optional<core::Vec2>& end,
                          const char* start_name, const char* end_name, std::uint32_t color) {
        if (!start || !end) {
            if (report) {
                scarlex::diagnostic_printf("[ESP]   skeleton link=%s->%s status=clipped\n", start_name, end_name);
            }
            return;
        }
        if (report) {
            if (drawn == 0) {
                scarlex::diagnostic_printf("[ESP] drawing skeleton for entity=0x%llX\n", address);
            }
            scarlex::diagnostic_printf("[ESP]   skeleton link=%s->%s status=drawn\n", start_name, end_name);
        }
        overlay.draw_line(start->x, start->y, end->x, end->y, color, settings.skeleton_thickness);
        ++drawn;
    };
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto parent = entity.bone_parents[i];
        if (parent < 0 || static_cast<std::size_t>(parent) >= points.size() || (i == legs && entity.limbs_valid)) {
            continue;
        }
        const auto color = i == legs ? settings.legs_color : i >= neck ? settings.head_color : settings.torso_color;
        link(points[parent], points[i], bone_names[parent], bone_names[i], color);
    }
    if (!entity.limbs_valid) {
        return drawn;
    }

    using game_entities::LimbJoint;
    constexpr auto pelvis = static_cast<std::size_t>(LimbJoint::pelvis);
    std::array<std::optional<core::Vec2>, game_entities::limb_joint_count> limbs{};
    for (std::size_t i = 0; i < limbs.size(); ++i) {
        const auto world = as_vector(entity.limb_joints[i]);
        limbs[i] = core::math::world_to_screen(world, matrix, width, height, depth);
        if (report) {
            scarlex::diagnostic_printf("[ESP]   joint_source=%s ", entity.measured_limb_joints.test(i) ? "Transform" : "estimated");
            log_point(limb_names[i], world, &matrix, width, height, true, true);
        }
    }
    link(points[root], limbs[pelvis], bone_names[root], limb_names[pelvis], settings.torso_color);
    const auto draw_limb = [&](LimbJoint first_joint, const std::optional<core::Vec2>& anchor,
                               const char* anchor_name, std::uint32_t color) {
        const auto start = static_cast<std::size_t>(first_joint);
        link(anchor, limbs[start], anchor_name, limb_names[start], color);
        for (std::size_t i = start + 1; i < start + 3; ++i) {
            link(limbs[i - 1], limbs[i], limb_names[i - 1], limb_names[i], color);
        }
    };
    draw_limb(LimbJoint::left_shoulder, points[spine], bone_names[spine], settings.arms_color);
    draw_limb(LimbJoint::right_shoulder, points[spine], bone_names[spine], settings.arms_color);
    draw_limb(LimbJoint::left_hip, limbs[pelvis], limb_names[pelvis], settings.legs_color);
    draw_limb(LimbJoint::right_hip, limbs[pelvis], limb_names[pelvis], settings.legs_color);
    return drawn;
}

}  // namespace

void ESPModule::on_render(const game_entities::GameEntities& snapshot, const OverlayD3D11& overlay,
                          float weapon_icon_size) const {
    if (!settings_.enabled) return;
    const auto& entities = snapshot.entities();
    const int width = overlay.width();
    const int height = overlay.height();
    if (width <= 0 || height <= 0) {
        if (diagnostics_) {
            scarlex::diagnostic_out() << "[ESP] viewport invalid: " << width << 'x' << height << '\n';
        }
        return;
    }

    const auto& camera = snapshot.camera();
    const core::CameraPose pose{as_vector(camera.position), as_vector(camera.forward),
                               as_vector(camera.right), as_vector(camera.up)};
    core::ProjectionParameters projection{fallback_fov, camera.near_plane, camera.far_plane, depth};
    if (std::isfinite(camera.fov) && camera.fov > 0.0f && camera.fov < 179.0f) {
        projection.fov = camera.fov;
    }
    const auto matrix = core::math::view_projection(pose, projection,
                                                    static_cast<float>(width) / static_cast<float>(height));
    static thread_local std::chrono::steady_clock::time_point next_log{};
    const auto now = diagnostics_ ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    const bool report = diagnostics_ && now >= next_log;
    if (report) {
        next_log = now + std::chrono::seconds(1);
        scarlex::diagnostic_printf("[ESP] MODULE IS CALLED! Entities count: %zu\n", entities.size());
        scarlex::diagnostic_printf("[ESP] viewport=%dx%d fov=%.2f (%s) camera=(%.3f,%.3f,%.3f) matrix=%s entities=%zu\n",
                    width, height, projection.fov, projection.fov == camera.fov ? "live" : "fallback",
                    pose.position.x, pose.position.y, pose.position.z, matrix ? "ok" : "invalid",
                    entities.size());
        if (!matrix) {
            scarlex::diagnostic_printf("[ESP] forward=(%.3f,%.3f,%.3f) right=(%.3f,%.3f,%.3f) "
                        "up=(%.3f,%.3f,%.3f) near=%.4f far=%.4f\n",
                        pose.forward.x, pose.forward.y, pose.forward.z, pose.right.x, pose.right.y, pose.right.z,
                        pose.up.x, pose.up.y, pose.up.z, projection.near_plane, projection.far_plane);
        }
    }

    const auto local_player = snapshot.local_player_address();
    const auto& local_position = snapshot.get_local_player_position();
    std::size_t drawn{};
    std::size_t skeleton_ready{};
    std::size_t skeleton_links{};
    for (const auto& entity : entities) {
        const float distance = local_position
            ? std::hypot(entity.position.x - local_position->x, entity.position.y - local_position->y,
                         entity.position.z - local_position->z) : entity.distance;
        const char* role_id = entity.role_id.empty() ? "unknown" : entity.role_id.c_str();
        const auto world_feet = as_vector(entity.position);
        const auto world_head = as_vector(entity.head_position);
        if (entity.skeleton_valid) {
            ++skeleton_ready;
        }
        if (report) {
            scarlex::diagnostic_printf("[ESP] entity=0x%llX local=%d valid_head=%d distance=%.2f skeleton=%s role=%s\n",
                        static_cast<unsigned long long>(entity.address), entity.address == local_player,
                        entity.valid, distance, entity.skeleton_valid ? "ok" : "unavailable",
                        role_id);
            scarlex::diagnostic_printf("[ESP] nickname=%s source=%s entity=0x%llX\n",
                        entity.nickname.empty() ? "(unavailable)" : entity.nickname.c_str(),
                        entity.nickname.empty() ? "unavailable" : entity.nickname_is_public_id ? "public-id" : "nickname",
                        static_cast<unsigned long long>(entity.address));
            scarlex::diagnostic_printf("[ESP] team_id=%d team_name=%s entity=0x%llX\n", entity.team_id,
                        entity.team_name.empty() ? "(unavailable)" : entity.team_name.c_str(),
                        static_cast<unsigned long long>(entity.address));
            const auto* view_projection = matrix ? &*matrix : nullptr;
            log_point("feet", world_feet, view_projection, width, height, true);
            log_point("head", world_head, view_projection, width, height, entity.valid);
        }
        const char* rejected = nullptr;
        if (!matrix) {
            rejected = "invalid-camera";
        } else if (!entity.valid) {
            rejected = "invalid-head";
        } else if (entity.address == local_player) {
            rejected = "local-player";
        } else if (!std::isfinite(distance) || distance < 0.0f) {
            rejected = "invalid-distance";
        } else if (distance > settings_.max_distance) {
            rejected = "distance-limit";
        }
        if (rejected) {
            if (report) {
                scarlex::diagnostic_printf("[ESP]   box=skipped reason=%s\n", rejected);
                if (entity.skeleton_valid) {
                    scarlex::diagnostic_printf("[ESP]   skeleton=skipped reason=%s\n", rejected);
                }
            }
            continue;
        }
        if (settings_.skeleton && entity.skeleton_valid) {
            skeleton_links += draw_skeleton(entity, *matrix, width, height, overlay, settings_, report);
        }
        const bool show_team = settings_.team_labels && (entity.team_id >= 0 || !entity.team_name.empty());
        if (!settings_.boxes && !settings_.role_labels && !show_team && !settings_.distance_labels && !settings_.weapon_labels &&
            !(settings_.esp_layout.name.visible && !entity.nickname.empty())) continue;
        const auto feet = core::math::world_to_screen(world_feet, *matrix, width, height, depth);
        const auto head = core::math::world_to_screen(world_head, *matrix, width, height, depth);
        if (!feet || !head) {
            if (report) {
                scarlex::diagnostic_printf("[ESP]   box=skipped reason=projection-rejected feet=%s head=%s\n",
                            feet ? "ok" : "clipped", head ? "ok" : "clipped");
            }
            continue;
        }

        const float top = (std::min)(head->y, feet->y);
        const float bottom = (std::max)(head->y, feet->y);
        const float box_height = bottom - top;
        if (box_height < 2.0f) {
            if (report) {
                scarlex::diagnostic_printf("[ESP]   box=skipped reason=height-too-small height=%.2f\n", box_height);
            }
            continue;
        }
        const float center_x = (head->x + feet->x) * 0.5f;
        const float half_width = (std::max)(box_height * 0.25f, std::abs(head->x - feet->x) * 0.5f + 2.0f);
        const float left = center_x - half_width;
        const float right = center_x + half_width;
        const auto color = role_color(entity.role, settings_);
        if (settings_.boxes) {
            if (settings_.box_outline) {
                overlay.draw_rect(left, top, right, bottom, outline, settings_.box_thickness + 1.5f);
            }
            overlay.draw_rect(left, top, right, bottom, color, settings_.box_thickness);
        }
        auto layout=settings_.esp_layout;
        std::string label,rarity;
        std::uint32_t weapon_color=0xFF3535FFu;
        ImTextureID icon{};
        auto* textures=overlay.texture_manager();
        const int mode=std::clamp(settings_.esp_weapon_display_mode,0,2);
        if (settings_.weapon_labels) {
            if (mode!=0 && textures && entity.has_weapon && !entity.weapon_id.empty()) icon=textures->WeaponIcon(entity.weapon_id);
            if (entity.weapon_readable && entity.has_weapon) {
                const auto metadata = game_data::lookup(entity.weapon_id);
                if (metadata.found()) {
                    label = game_data::display_name(metadata, true);
                    if (metadata.item) rarity=metadata.item->rarity; // Player ESP never displays the item's price.
                    weapon_color = game_data::rarity_color(metadata,
                        item_presentation::display_color(entity.weapon_rarity, false, weapon_color));
                } else game_data::report_unmatched(entity.weapon_id, metadata, "esp-weapon");
                if (!label.empty() && settings_.weapon_ammo && entity.weapon_ammo >= 0) {
                    label += " [" + std::to_string(entity.weapon_ammo);
                    if (entity.weapon_reserve_ammo >= 0) label += "/" + std::to_string(entity.weapon_reserve_ammo);
                    label += "]";
                }
            } else if (entity.weapon_readable && settings_.show_unarmed) {
                label = "Unarmed";
                weapon_color = 0xFFE4E4E4u;
            }
        }
        char distance_text[48]{};
        std::snprintf(distance_text,sizeof(distance_text),"%.0f m",static_cast<double>(distance));
        game_entities::EspItemSizes sizes{};
        const auto text_size=[&](std::size_t i,std::string_view text,bool visible) {
            const auto measured=overlay.measure_text(text);
            sizes[i]={measured.x,measured.y,visible && !text.empty()};
        };
        std::string role_and_team;
        if (settings_.role_labels && layout.role.visible) role_and_team = role_id;
        if (show_team) {
            layout.role.visible = true;
            if (!role_and_team.empty()) role_and_team += "  ";
            if (!entity.team_name.empty()) {
                role_and_team += '[';
                role_and_team += entity.team_name;
                role_and_team += ']';
            }
            if (entity.team_id >= 0) {
                if (!entity.team_name.empty()) role_and_team += ' ';
                role_and_team += '#';
                role_and_team += std::to_string(entity.team_id);
            }
        }
        text_size(0,role_and_team,!role_and_team.empty());
        text_size(1,label,settings_.weapon_labels && (mode!=1 || !icon));
        text_size(2,distance_text,settings_.distance_labels);
        const auto name_item = static_cast<std::size_t>(game_entities::EspItem::name);
        text_size(name_item,entity.nickname,layout.name.visible);
        const auto title_size=overlay.measure_text(label),rarity_size=overlay.measure_text(rarity);
        if (!rarity.empty()) {
            sizes[1].width=(std::max)(sizes[1].width,rarity_size.x);
            sizes[1].height+=2+rarity_size.y;
        }
        if (icon) {const auto s=textures->IconSize(icon,weapon_icon_size);sizes[4]={s.x,s.y,true};}
        const auto positions=game_entities::arrange_esp_layout(layout,sizes,left,top,right,bottom);
        const auto draw_label=[&](std::size_t i,std::string_view text,std::uint32_t tint) {
            if(positions[i].visible) overlay.draw_text(positions[i].center().x,positions[i].y,text,tint);
        };
        draw_label(0,role_and_team,color);draw_label(2,distance_text,settings_.distance_color);
        draw_label(name_item,entity.nickname,color);
        draw_label(1,label,weapon_color);
        if (positions[1].visible && !rarity.empty())
            overlay.draw_text(positions[1].center().x,positions[1].y+title_size.y+2,rarity,weapon_color);
        if (positions[4].visible) {
            const auto& r=positions[4];
            ImGui::GetBackgroundDrawList()->AddImage(icon,{r.x,r.y},{r.x+r.width,r.y+r.height});
        }
        ++drawn;
        if (report) {
            const char* nickname_display = positions[name_item].visible ? "drawn"
                : layout.name.visible ? "unavailable" : "hidden-by-layout";
            scarlex::diagnostic_printf("[ESP]   nickname_display=%s\n", nickname_display);
            scarlex::diagnostic_printf("[ESP]   box=drawn bounds=(%.2f,%.2f)-(%.2f,%.2f)\n", left, top, right, bottom);
        }
    }
    if (report) {
        scarlex::diagnostic_printf("[ESP] frame summary drawn=%zu total=%zu skeleton_ready=%zu skeleton_links=%zu\n",
                    drawn, entities.size(), skeleton_ready, skeleton_links);
        std::fflush(stdout);
    }
}

