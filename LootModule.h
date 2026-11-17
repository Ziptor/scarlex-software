#pragma once

#include "game_data.h"

#include <cstdint>
#include <unordered_set>
#include <utility>

class OverlayD3D11;

namespace game_entities { class GameEntities; }

struct LootSettings {
    bool enabled = true;
    bool bags = true;
    bool containers = true;
    bool cargo = true;
    bool weapons = true;
    bool ground_items = true;
    bool vehicles = true;
    bool show_contents = true;
    bool markers = true;
    bool names = true;
    bool distance = true;
    bool hide_empty = false;
    bool show_price = true;
    float max_distance = 150.0f;
    float min_price = 0.0f;
    float marker_size = 3.0f;
    std::uint32_t bag_color = 0xFF42C8FFu;
    std::uint32_t container_color = 0xFFFFFFFFu;
    std::uint32_t cargo_color = 0xFF3535FFu;
    std::uint32_t weapon_color = 0xFF3535FFu; // Retained for old profiles. Rendering enforces red.
    std::uint32_t ground_color = 0xFFE4D7AFu;
    std::uint32_t vehicle_color = 0xFFFFC595u;
    std::unordered_set<std::string> disabled_items;

    [[nodiscard]] bool item_enabled(const game_data::Match& item) const {
        return (!item.item || item.item->enabled_by_user) && !disabled_items.contains(item.normalized_id);
    }
};

class LootModule final {
public:
    explicit LootModule(LootSettings settings = {}) : settings_(std::move(settings)) {}
    explicit LootModule(float max_distance) : settings_{.max_distance = max_distance} {}
    void on_render(const game_entities::GameEntities& snapshot, const OverlayD3D11& overlay) const;
    static void render(const game_entities::GameEntities& snapshot, const OverlayD3D11& overlay,
                       const LootSettings& settings, game_data::Language language);

private:
    LootSettings settings_;
};
