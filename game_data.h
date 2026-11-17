#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace game_data {

enum class Language { english, russian };

struct ItemData {
    std::string id, name_ru, name_en, category, rarity;
    std::optional<double> price_min, price_max;
    bool enabled_by_user = true;
};
struct WeaponData {
    std::string id, name_ru, name_en;
    std::optional<double> damage, headshot_damage, magazine, dps;
};
struct MapPointData {
    std::string id, type, name_ru, name_en, district_ru, building_ru;
    std::optional<double> loot_entries;
};
struct Catalog {
    std::unordered_map<std::string, ItemData> items;
    std::unordered_map<std::string, WeaponData> weapons;
    std::unordered_map<std::string, MapPointData> map_points;
    [[nodiscard]] bool contains(const std::string& key) const;
};

[[nodiscard]] const Catalog& catalog();

// Extend this table as new runtime IDs are identified. Values must be actual CSV keys.
inline const std::unordered_map<std::string, std::string> overrides{
    {"garbage1-1", "garbage_1"},
    {"garbage1-2", "garbage_1"},
    {"event-atm", "atm"},
    {"event-safe", "safe"},
    // Asset manifest IDs -> original CSV IDs; matched by exact weapon display name.
    {"lmg_m2", "m249_m2"},
    {"drm14_m2", "weapon_drm14_mk2"},
    {"drm14_m3", "weapon_drm14_mk3"},
    {"tag12k_m1", "tag12_m1"},
    {"tag12k_m2", "weapon_tag12_mk2"},
    {"tag12k_m3", "weapon_tag12_mk3"},
    {"vulture50_m1", "vulture_m1"},
    {"vulture50_m2", "vulture_m2"},
    {"lbr44_m2", "weapon_lbr44_mk2"},
    {"dp9_m2", "weapon_dp9_mk2"},
};

struct Match {
    std::string normalized_id;
    bool override_applied{};
    const ItemData* item{};
    const WeaponData* weapon{};
    const MapPointData* point{};
    [[nodiscard]] bool found() const noexcept { return item || weapon || point; }
};

// Remove event-, at most two numeric suffix groups, then replace remaining hyphens.
// Only a missing cleaned key falls back to the RAW-key overrides table.
[[nodiscard]] std::string normalize_id(const std::string& raw);
[[nodiscard]] Match lookup(const std::string& raw);
[[nodiscard]] std::string display_name(const Match& match, bool prefer_weapon = false,
                                       Language language = Language::russian);
[[nodiscard]] std::string details(const Match& match, bool include_price = true,
                                   Language language = Language::russian);
[[nodiscard]] std::string price_label(const Match& match, Language language = Language::russian);
[[nodiscard]] std::optional<double> filter_price(const Match& match) noexcept;
[[nodiscard]] const char* rarity_name(int rarity, Language language) noexcept;
[[nodiscard]] int rarity_value(const Match& match) noexcept;
[[nodiscard]] std::uint32_t rarity_color(const Match& match, std::uint32_t fallback) noexcept;

// Thread-safe, one entry per distinct raw ID for this process session. Writes beside the EXE.
void report_unmatched(const std::string& raw, const Match& match, const char* context);
[[nodiscard]] std::vector<std::string> unmatched_ids();

} // namespace game_data
