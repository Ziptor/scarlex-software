#pragma once
#include "release_build.h"

#include "MeshGeometry.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class WmPortReader;

namespace game_entities {

enum class EspAnchor : std::uint8_t { top_center, bottom_center, top_left, top_right };

struct EspElementLayout {
    EspAnchor anchor = EspAnchor::bottom_center;
    bool visible = true;
    bool operator==(const EspElementLayout&) const = default;
};

struct EspLayout {
    EspElementLayout role{EspAnchor::bottom_center,true};
    EspElementLayout weapon{EspAnchor::bottom_center,true};
    EspElementLayout distance{EspAnchor::top_center,true};
    EspElementLayout name{EspAnchor::top_center,true};
    EspElementLayout weapon_icon{EspAnchor::top_right,true};
    bool operator==(const EspLayout&) const = default;
};

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

enum class SkeletonBone : std::uint8_t { root, legs, spine2, spine3, neck, head1, head2, count };
inline constexpr std::size_t skeleton_bone_count = static_cast<std::size_t>(SkeletonBone::count);

enum class LimbJoint : std::uint8_t {
    pelvis,
    left_shoulder, left_elbow, left_hand, right_shoulder, right_elbow, right_hand,
    left_hip, left_knee, left_foot, right_hip, right_knee, right_foot,
    count
};
inline constexpr std::size_t limb_joint_count = static_cast<std::size_t>(LimbJoint::count);

enum class CharacterRole : std::uint8_t { unknown, neutral, cop, robber };

struct Entity {
    std::uint64_t address{};
    Vec3 position{};
    Vec3 head_position{};
    float distance{};
    bool valid{};  // A live presentation view supplied a world-space head position.
    std::array<Vec3, skeleton_bone_count> bones{};
    std::array<std::int8_t, skeleton_bone_count> bone_parents{}; // Semantic indices. Root has -1.
    bool skeleton_valid{};
    std::array<Vec3, limb_joint_count> limb_joints{};
    std::bitset<limb_joint_count> measured_limb_joints{};
    bool limbs_valid{};
    MeshPose mesh;
    CharacterRole role = CharacterRole::unknown;
    std::string role_id;  // UTF-8 PlayerRoleDescription.Id. Empty if unreadable.
    Vec3 prev_head_position{};
    Vec3 velocity{};  // Estimated world-space velocity in meters per second.
    std::string weapon_name;
    std::string weapon_id;
    int weapon_ammo = -1;
    int weapon_reserve_ammo = -1;
    int weapon_rarity = -1;
    bool weapon_readable = false;
    bool has_weapon = false;
    std::string nickname; // UTF-8 display nickname or decimal PublicId fallback.
    bool nickname_is_public_id{};
    int team_id = -1; // ClanBattleTeamDescription.Index, when a battle side is known.
    std::string team_name; // Clan name, clan tag, or the battle side's name.
};

struct Camera {
    Vec3 position{};
    Vec3 forward{};
    Vec3 right{};
    Vec3 up{};
    float fov{};  // Vertical degrees. Zero means the live lens is unavailable.
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
};

struct Vehicle {
    std::int32_t entity_id{};
    std::uint64_t address{};  // VehicleClientModel reference from the dictionary.
};

enum class LootKind { bag, container, cargo, weapon, ground_item, vehicle };

struct LootStack {
    std::string name;
    std::uint32_t count{};
    LootKind type = LootKind::ground_item;
    int rarity = -1;
    std::string name_clean;
};

struct LootItem {
    std::int32_t entity_id{};
    std::uint64_t address{};
    LootKind type{};
    Vec3 position{};
    float distance{};
    std::string name;
    // Disengaged if the inventory has not been replicated or could not be read.
    std::optional<std::vector<LootStack>> contents;
    int rarity = -1;
    std::string name_clean;
};

struct LootNode {
    std::int32_t runtime_id{};
    std::uint64_t address{};
    std::uint64_t definition_address{};
    std::string definition_id;
    std::int32_t definition_entity_id{};
    std::int32_t definition_index = -1;
    Vec3 position{};
    bool position_valid{};
    bool position_from_interact_point{};
    // The provided dumps contain no donor/building/district fields.
    std::string donor_id;
    std::string name_ru;
    std::string building_ru;
    std::string district_ru;
};

// x64 IL2CPP Dictionary<int32_t, managed-reference>, starting at object +0x10.
struct Dictionary {
    std::uint64_t buckets{};
    std::uint64_t entries{};
    std::int32_t count{};
    std::int32_t free_list{};
    std::int32_t free_count{};
    std::int32_t version{};

    bool operator==(const Dictionary&) const = default;

    struct Entry {
        std::uint32_t hash_code{};
        std::int32_t next{};
        std::int32_t entity_id{};
        // Four bytes of alignment padding precede this x64 managed reference.
        std::uint64_t value{};
    };
};

class GameEntities final {
public:
    // Supply a validated global-slot RVA, not the manager's heap address.
    explicit GameEntities(std::uint64_t gom_rva = 0x2230768) noexcept : gom_rva_(gom_rva) {}

    // Use the managed UnityEntry object resolved for the PID passed to update().
    [[nodiscard]] static GameEntities from_unity_entry(std::uint64_t address) noexcept;

    // Single-threaded snapshot. A failed update invalidates the previous snapshot.
    [[nodiscard]] bool update(WmPortReader& reader, std::uint32_t pid);
    void set_diagnostics(bool enabled) noexcept { diagnostics_ = enabled && scarlex::build::diagnostics; }
    void set_mesh_overlay(bool enabled) noexcept { mesh_overlay_ = enabled; }
    // Move only published frame data. Structure and read caches stay on the producer.
    void swap_frame(GameEntities& other) noexcept;

    [[nodiscard]] const std::vector<Entity>& entities() const noexcept { return entities_; }
    [[nodiscard]] const std::vector<Vehicle>& vehicles() const noexcept { return vehicles_; }
    [[nodiscard]] const std::vector<LootItem>& loot_items() const noexcept { return loot_items_; }
    [[nodiscard]] const std::vector<LootNode>& loot_nodes() const noexcept { return loot_nodes_; }
    [[nodiscard]] std::uint64_t local_player_address() const noexcept { return local_player_address_; }
    // World-space origin selected by the producer each snapshot: character or occupied vehicle.
    [[nodiscard]] const std::optional<Vec3>& get_local_player_position() const noexcept { return local_player_position_; }
    [[nodiscard]] const std::optional<Vec3>& local_player_position() const noexcept { return get_local_player_position(); }
    [[nodiscard]] const Camera& camera() const noexcept { return camera_; }

private:
    struct Cache;
    [[nodiscard]] bool update_once(WmPortReader& reader, std::uint32_t pid);
    std::uint64_t gom_rva_{};
    std::optional<std::uint64_t> unity_entry_;
    bool diagnostics_ = scarlex::build::diagnostics;
    bool mesh_overlay_{};
    std::shared_ptr<Cache> cache_;
    std::vector<Entity> entities_;
    std::vector<Vehicle> vehicles_;
    std::vector<LootItem> loot_items_;
    std::vector<LootNode> loot_nodes_;
    std::uint64_t local_player_address_{};
    std::optional<Vec3> local_player_position_;
    Camera camera_{};
};

}  // namespace game_entities
