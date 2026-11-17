#include "ItemPresentation.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace item_presentation {
namespace {

std::string lowercase(std::string_view input) {
    std::string value(input);
    for (auto& c : value) {
        if (static_cast<unsigned char>(c) < 128) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return value;
}

bool digits(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

std::string clean_name(std::string_view id, std::string_view fallback_id) {
    const auto first = id.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return fallback_id.empty() ? "Unknown" : "Item [" + std::string(fallback_id) + "]";
    }
    id = id.substr(first, id.find_last_not_of(" \t\r\n") - first + 1);
    if (digits(id)) return "Item [" + std::string(id) + "]";
    std::string value = lowercase(id);
    const auto marker = value.rfind("_m");
    if (marker != std::string::npos && digits(std::string_view(value).substr(marker + 2))) {
        value.resize(marker);
    }
    struct Alias { std::string_view prefix, label; };
    constexpr std::array aliases{
        Alias{"rce556", "Rifle 5.56"}, Alias{"rc556", "Rifle 5.56"}, Alias{"lmg", "LMG"},
        Alias{"reagents", "Reagents"}, Alias{"rifle_ammo", "Rifle ammo"},
        Alias{"pistol_ammo", "Pistol ammo"}, Alias{"shotgun_ammo", "Shotgun ammo"},
        Alias{"mp_ammo", "SMG ammo"},
    };
    for (const auto& alias : aliases) {
        if (value == alias.prefix || (value.starts_with(alias.prefix) &&
            value.size() > alias.prefix.size() && value[alias.prefix.size()] == '_')) {
            return std::string(alias.label);
        }
    }
    std::string result;
    bool new_word = true;
    for (unsigned char c : value) {
        if (c == '_' || c == '-' || c == ' ' || c == '\t') {
            if (!result.empty() && result.back() != ' ') result += ' ';
            new_word = true;
        } else if (c >= 32 && c != 127) {
            result += new_word && c < 128 ? static_cast<char>(std::toupper(c)) : static_cast<char>(c);
            new_word = false;
        }
    }
    if (!result.empty() && result.back() == ' ') result.pop_back();
    if (result.empty()) return fallback_id.empty() ? "Unknown" : "Item [" + std::string(fallback_id) + "]";
    return result;
}

bool weapon_name(std::string_view name) {
    const auto id = lowercase(name);
    return id.find("weapon") != std::string::npos || id.find("gun") != std::string::npos;
}

std::uint32_t display_color(int rarity, bool weapon, std::uint32_t fallback) noexcept {
    if (weapon) return 0xFF3535FFu;
    switch (static_cast<Rarity>(rarity)) {
    case Rarity::common: return 0xFFE4E4E4u;
    case Rarity::uncommon: return 0xFF67D56Bu;
    case Rarity::rare: return 0xFFFF914Du;
    case Rarity::epic: return 0xFFF16DBCu;
    case Rarity::legendary: return 0xFF39C8FFu;
    case Rarity::exotic: return 0xFF329CFFu;
    default: return fallback;
    }
}

}  // namespace item_presentation
