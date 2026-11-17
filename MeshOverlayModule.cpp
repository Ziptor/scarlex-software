#include "runtime_log.h"
#include "MeshOverlayModule.h"

#include "MeshGeometry.h"
#include "OverlayD3D11.h"
#include "core/math.h"
#include "game_entities.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

namespace {

using game_entities::LimbJoint;
using game_entities::SkeletonBone;
using game_entities::Vec3;
using game_entities::MeshPose;

core::Vec3 as_vector(Vec3 v) noexcept { return {v.x, v.y, v.z}; }

bool skin(const MeshPose& pose, const core::Mat4& projection, std::vector<core::Vec4>& clip) {
    if (!pose.geometry || pose.geometry->positions.empty()) {
        return false;
    }
    const auto& mesh = *pose.geometry;
    clip.clear();
    clip.reserve(mesh.positions.size());
    if (mesh.inverse_bind.empty()) {
        if (pose.bone_to_world.size() != 1) {
            return false;
        }
        for (const auto& vertex : mesh.positions) {
            const auto p = transform(pose.bone_to_world.front(), vertex);
            clip.push_back(core::math::transform(projection, {p[0], p[1], p[2]}));
        }
        return true;
    }
    if (mesh.inverse_bind.size() != pose.bone_to_world.size() ||
        mesh.weight_offsets.size() != mesh.positions.size() + 1 ||
        mesh.weight_offsets.front() != 0 || mesh.weight_offsets.back() != mesh.weights.size()) {
        return false;
    }
    std::vector<game_entities::MeshAffine> palette;
    palette.reserve(mesh.inverse_bind.size());
    for (std::size_t i = 0; i < mesh.inverse_bind.size(); ++i) {
        palette.push_back(multiply(pose.bone_to_world[i], mesh.inverse_bind[i]));
    }
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        core::Vec3 world{};
        float sum{};
        const auto begin = mesh.weight_offsets[i], end = mesh.weight_offsets[i + 1];
        if (begin >= end || end > mesh.weights.size()) {
            return false;
        }
        for (std::size_t k = begin; k < end; ++k) {
            const auto [weight, bone] = mesh.weights[k];
            if (bone >= palette.size() || !std::isfinite(weight) || weight < 0.0f) {
                return false;
            }
            const auto p = transform(palette[bone], mesh.positions[i]);
            world.x += weight * p[0];
            world.y += weight * p[1];
            world.z += weight * p[2];
            sum += weight;
        }
        if (!std::isfinite(sum) || sum <= 0.0f || !core::math::finite(world)) {
            return false;
        }
        clip.push_back(core::math::transform(projection, {world.x / sum, world.y / sum, world.z / sum}));
    }
    return true;
}

class Capsules {
public:
    Capsules(const core::Mat4& projection, int width, int height)
        : projection_(projection), width_(static_cast<float>(width)), height_(static_cast<float>(height)) {}

    void add(Vec3 start, Vec3 end, float radius) {
        auto a = core::math::transform(projection_, as_vector(start));
        auto b = core::math::transform(projection_, as_vector(end));
        if (!clip_depth(a, b)) {
            return;
        }
        const float x = (b.x / b.w - a.x / a.w) * width_ * 0.5f;
        const float y = (b.y / b.w - a.y / a.w) * height_ * 0.5f;
        const float length = std::hypot(x, y);
        if (!std::isfinite(length)) {
            return;
        }
        if (length <= 0.01f) {
            disk(a, radius);
            return;
        }
        const float dx = -y / length * radius;
        const float dy = x / length * radius;
        const auto base = static_cast<std::uint32_t>(vertices.size());
        vertices.insert(vertices.end(), {shift(a, dx, dy), shift(a, -dx, -dy),
                                         shift(b, dx, dy), shift(b, -dx, -dy)});
        indices.insert(indices.end(), {base, base + 1, base + 2, base + 2, base + 1, base + 3});
        disk(a, radius);
        disk(b, radius);
    }

    std::vector<core::Vec4> vertices;
    std::vector<std::uint32_t> indices;

private:
    static bool clip_depth(core::Vec4& a, core::Vec4& b) {
        if (!std::isfinite(a.z) || !std::isfinite(b.z) || (a.z < 0.0f && b.z < 0.0f)) {
            return false;
        }
        auto* outside = a.z < 0.0f ? &a : b.z < 0.0f ? &b : nullptr;
        if (outside) {
            const auto& inside = outside == &a ? b : a;
            const float t = -outside->z / (inside.z - outside->z);
            *outside = {std::lerp(outside->x, inside.x, t), std::lerp(outside->y, inside.y, t),
                        0.0f, std::lerp(outside->w, inside.w, t)};
        }
        return a.w > 1.0e-5f && b.w > 1.0e-5f;
    }

    core::Vec4 shift(core::Vec4 point, float x, float y) const {
        point.x += 2.0f * x / width_ * point.w;
        point.y += 2.0f * y / height_ * point.w;
        return point;
    }

    void disk(core::Vec4 center, float radius) {
        constexpr std::uint32_t segments = 10;
        const auto base = static_cast<std::uint32_t>(vertices.size());
        vertices.push_back(center);
        for (std::uint32_t i = 0; i < segments; ++i) {
            const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / segments;
            vertices.push_back(shift(center, radius * std::cos(angle), radius * std::sin(angle)));
            indices.insert(indices.end(), {base, base + 1 + i, base + 1 + (i + 1) % segments});
        }
    }

    const core::Mat4& projection_;
    float width_, height_;
};

void capsule_pose(const game_entities::Entity& entity, const core::Mat4& matrix,
                  int width, int height, Capsules& out) {
    constexpr auto depth = core::ClipDepthRange::zero_to_one;
    const auto feet = core::math::world_to_screen(as_vector(entity.position), matrix, width, height, depth);
    const auto head = core::math::world_to_screen(as_vector(entity.head_position), matrix, width, height, depth);
    const float radius = feet && head ? std::clamp(std::abs(feet->y - head->y) * 0.062f, 2.0f, 36.0f)
                                      : std::clamp(static_cast<float>(height) / (entity.distance + 4.0f) * 0.075f,
                                                   2.0f, 36.0f);
    if (!entity.skeleton_valid) {
        out.add(entity.position, entity.head_position, radius);
        return;
    }
    for (std::size_t i = 0; i < entity.bones.size(); ++i) {
        const int parent = entity.bone_parents[i];
        if (parent >= 0 && static_cast<std::size_t>(parent) < entity.bones.size() &&
            (!entity.limbs_valid || i != static_cast<std::size_t>(SkeletonBone::legs))) {
            out.add(entity.bones[static_cast<std::size_t>(parent)], entity.bones[i], radius);
        }
    }
    if (!entity.limbs_valid) {
        return;
    }
    const auto joint = [&](LimbJoint j) { return entity.limb_joints[static_cast<std::size_t>(j)]; };
    const auto pelvis = joint(LimbJoint::pelvis);
    const auto spine = entity.bones[static_cast<std::size_t>(SkeletonBone::spine3)];
    out.add(pelvis, entity.bones[static_cast<std::size_t>(SkeletonBone::spine2)], radius);
    constexpr std::array chains{
        std::array{LimbJoint::left_shoulder, LimbJoint::left_elbow, LimbJoint::left_hand},
        std::array{LimbJoint::right_shoulder, LimbJoint::right_elbow, LimbJoint::right_hand},
        std::array{LimbJoint::left_hip, LimbJoint::left_knee, LimbJoint::left_foot},
        std::array{LimbJoint::right_hip, LimbJoint::right_knee, LimbJoint::right_foot},
    };
    for (std::size_t i = 0; i < chains.size(); ++i) {
        auto previous = i < 2 ? spine : pelvis;
        for (const auto bone : chains[i]) {
            const auto next = joint(bone);
            out.add(previous, next, radius * (i < 2 ? 0.55f : 0.75f));
            previous = next;
        }
    }
}

}  // namespace

void MeshOverlayModule::on_render(const game_entities::GameEntities& snapshot, const OverlayD3D11& overlay) const {
    const int width = overlay.width(), height = overlay.height();
    if (width <= 0 || height <= 0) {
        return;
    }
    const auto& camera = snapshot.camera();
    const auto matrix = core::math::view_projection(
        {as_vector(camera.position), as_vector(camera.forward), as_vector(camera.right), as_vector(camera.up)},
        {camera.fov > 0.0f && camera.fov < 179.0f ? camera.fov : 90.0f,
         camera.near_plane, camera.far_plane, core::ClipDepthRange::zero_to_one},
        static_cast<float>(width) / static_cast<float>(height));
    if (!matrix) {
        return;
    }
    std::vector<core::Vec4> clip;
    Capsules capsules(*matrix, width, height);
    std::size_t meshes{}, fallbacks{};
    for (const auto& entity : snapshot.entities()) {
        if (!entity.valid || !std::isfinite(entity.distance) || entity.distance < 0.0f ||
            entity.distance > max_distance_ ||
            entity.address == snapshot.local_player_address()) {
            continue;
        }
        if (skin(entity.mesh, *matrix, clip)) {
            overlay.draw_mesh_clip(clip, entity.mesh.geometry->indices, color_);
            ++meshes;
        } else if (capsules_) {
            capsule_pose(entity, *matrix, width, height, capsules);
            ++fallbacks;
        }
    }
    overlay.draw_mesh_clip(capsules.vertices, capsules.indices, color_);
    if (diagnostics_) {
        static thread_local std::chrono::steady_clock::time_point next_log{};
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_log) {
            next_log = now + std::chrono::seconds(1);
            scarlex::diagnostic_printf("[MESH] mesh=%zu capsules=%zu\n", meshes, fallbacks);
        }
    }
}

