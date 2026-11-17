#include "runtime_log.h"
#include "game_data.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>

namespace game_data {
namespace {

std::string clean_id(std::string raw) {
    if (raw.starts_with("event-")) raw.erase(0, 6);
    for (unsigned group = 0; group < 2; ++group) {
        const auto dash = raw.find_last_of('-');
        if (dash == std::string::npos || dash + 1 == raw.size()) break;
        if (!std::all_of(raw.begin() + static_cast<std::ptrdiff_t>(dash + 1), raw.end(),
                        [](unsigned char c) { return c >= '0' && c <= '9'; })) break;
        raw.resize(dash);
    }
    std::replace(raw.begin(), raw.end(), '-', '_');
    return raw;
}

std::string escaped(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
        } else out += static_cast<char>(c);
    }
    return out + '"';
}

struct SessionLog {
    std::mutex mutex;
    std::unordered_set<std::string> raw_ids;
    std::ofstream file;
    bool attempted{};
    void open() {
        if (attempted) return;
        attempted = true;
        std::wstring path(32768, L'\0');
        const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size || size >= path.size()) return;
        path.resize(size);
        const auto target = std::filesystem::path(path).parent_path() / L"unmatched-ids.jsonl";
        file.open(target, std::ios::binary | std::ios::trunc);
        if (!file) scarlex::diagnostic_printf("[ID-MAP] Cannot open unmatched-ids.jsonl beside the EXE\n");
    }
};
SessionLog& session_log() { static SessionLog value; return value; }

std::string price(double value) {
    char text[64]{};
    std::snprintf(text, sizeof(text), "%.0f", value);
    return text;
}

} // namespace

bool Catalog::contains(const std::string& key) const {
    return items.contains(key) || weapons.contains(key) || map_points.contains(key);
}

const Catalog& catalog() {
    static const Catalog value = [] {
        Catalog result;
        // Generated from the supplied UTF-8 CSVs. No CSV files are required at runtime.
#include "game_data_entries.inl"
        return result;
    }();
    return value;
}

Match lookup(const std::string& raw) {
    Match match;
    match.normalized_id = clean_id(raw);
    const auto& data = catalog();
    if (!data.contains(match.normalized_id)) {
        if (const auto it = overrides.find(raw); it != overrides.end()) {
            match.normalized_id = it->second;
            match.override_applied = true;
        }
    }
    if (!match.normalized_id.empty()) {
        if (const auto it = data.items.find(match.normalized_id); it != data.items.end()) match.item = &it->second;
        if (const auto it = data.weapons.find(match.normalized_id); it != data.weapons.end()) match.weapon = &it->second;
        if (const auto it = data.map_points.find(match.normalized_id); it != data.map_points.end()) match.point = &it->second;
    }
    return match;
}

std::string normalize_id(const std::string& raw) { return lookup(raw).normalized_id; }

std::string display_name(const Match& match, bool prefer_weapon, Language language) {
    const auto name = [language](const auto* value) -> std::string {
        if (!value) return {};
        if (language == Language::english) return value->name_en.empty() ? value->name_ru : value->name_en;
        return value->name_ru.empty() ? value->name_en : value->name_ru;
    };
    if (prefer_weapon) { auto value = name(match.weapon); if (!value.empty()) return value; }
    if (auto value = name(match.item); !value.empty()) return value;
    if (auto value = name(match.point); !value.empty()) {
        if (language == Language::russian && !match.point->building_ru.empty()) {
            value += " · " + match.point->building_ru;
        }
        return value;
    }
    if (auto value = name(match.weapon); !value.empty()) return value;
    // A known CSV row with no localization still has a useful exact identifier.
    return match.found() ? match.normalized_id : std::string{};
}

std::string price_label(const Match& match, Language language) {
    if (!match.item) return {};
    const auto& low = match.item->price_min;
    const auto& high = match.item->price_max;
    if (low && high) {
        auto out = "$" + price(*low);
        if (*low != *high) out += "–" + price(*high);
        return out;
    }
    if (low) return std::string(language == Language::russian ? "от $" : "from $") + price(*low);
    if (high) return std::string(language == Language::russian ? "до $" : "up to $") + price(*high);
    return {};
}

std::optional<double> filter_price(const Match& match) noexcept {
    if (!match.item) return {};
    return match.item->price_min ? match.item->price_min : match.item->price_max;
}

std::string details(const Match& match, bool include_price, Language language) {
    if (!match.item) return {};
    const int rarity = rarity_value(match);
    std::string out = rarity >= 1 ? rarity_name(rarity, language) : match.item->rarity;
    if (include_price) {
        const auto value = price_label(match, language);
        if (!value.empty()) {
            if (!out.empty()) out += " · ";
            out += value;
        }
    }
    return out;
}

int rarity_value(const Match& match) noexcept {
    if (!match.item) return -1;
    const auto& value = match.item->rarity;
    if (value == "Обычный" || value == "Common") return 1;
    if (value == "Необычный" || value == "Uncommon") return 2;
    if (value == "Редкий" || value == "Rare") return 3;
    if (value == "Эпический" || value == "Epic") return 4;
    if (value == "Легендарный" || value == "Legendary") return 5;
    if (value == "Экзотический" || value == "Exotic") return 6;
    return -1;
}

const char* rarity_name(int rarity, Language language) noexcept {
    constexpr const char* ru[]{"Неизвестная", "Обычный", "Необычный", "Редкий", "Эпический", "Легендарный", "Экзотический"};
    constexpr const char* en[]{"Unknown", "Common", "Uncommon", "Rare", "Epic", "Legendary", "Exotic"};
    const int index = rarity >= 1 && rarity <= 6 ? rarity : 0;
    return language == Language::russian ? ru[index] : en[index];
}

std::uint32_t rarity_color(const Match& match, std::uint32_t fallback) noexcept {
    switch (rarity_value(match)) {
    case 1: return 0xFFE4E4E4u;
    case 2: return 0xFF67D56Bu;
    case 3: return 0xFFFFA64Du;
    case 4: return 0xFFD08CDBu;
    case 5: return 0xFF32B6FFu;
    case 6: return 0xFF3535FFu;
    default: return fallback;
    }
}

void report_unmatched(const std::string& raw, const Match& match, const char* context) {
    if constexpr (!scarlex::build::diagnostics) return;
    if (match.found()) return;
    auto& log = session_log();
    std::lock_guard lock(log.mutex);
    if (!log.raw_ids.insert(raw).second) return;
    log.open();
    const auto raw_text = escaped(raw), normalized = escaped(match.normalized_id);
    const std::string record = "{\"status\":\"unknown\",\"raw_id\":" + raw_text +
        ",\"normalized_id\":" + normalized + ",\"override_applied\":" +
        (match.override_applied ? "true" : "false") + ",\"context\":" + escaped(context ? context : "") + "}";
    scarlex::diagnostic_printf("[ID-MAP] unmatched %s\n", record.c_str());
    if (log.file) { log.file << record << '\n'; log.file.flush(); }
}

std::vector<std::string> unmatched_ids() {
    auto& log = session_log();
    std::lock_guard lock(log.mutex);
    std::vector<std::string> values(log.raw_ids.begin(),log.raw_ids.end());
    std::sort(values.begin(),values.end());
    return values;
}

} // namespace game_data

