#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace item_presentation {

// RarityItem values in the current metadata. -1 represents an unavailable read.
enum class Rarity : int {
    unknown = -1, none = 0, common = 1, uncommon = 2, rare = 3,
    epic = 4, legendary = 5, exotic = 6
};

[[nodiscard]] std::string clean_name(std::string_view id, std::string_view fallback_id = {});
[[nodiscard]] bool weapon_name(std::string_view name);
[[nodiscard]] std::uint32_t display_color(int rarity, bool weapon, std::uint32_t fallback) noexcept;

}  // namespace item_presentation
