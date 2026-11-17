#pragma once

class OverlayD3D11;
namespace game_entities { class GameEntities; }

struct ArrowIndicatorSettings {
    bool enabled = true;
    float arrow_radius = 80.0f;
    float arrow_size = 10.0f;
    float max_distance = 200.0f;
    bool show_only_offscreen = false;
    bool arrow_show_weapon_icon = true;
};

class ArrowIndicatorModule final {
public:
    explicit ArrowIndicatorModule(ArrowIndicatorSettings settings = {}) noexcept;
    void on_render(const game_entities::GameEntities& snapshot, const OverlayD3D11& overlay,
                   bool menu_open, float weapon_icon_size = 48.0f) const;

private:
    ArrowIndicatorSettings settings_;
};
