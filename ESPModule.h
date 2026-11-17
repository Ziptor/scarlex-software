#pragma once

#include <cstdint>
#include <limits>
#include "game_entities.h"

class OverlayD3D11;

namespace game_entities {
class GameEntities;
}

struct ESPSettings {
    bool enabled = true;
    bool boxes = true;
    bool box_outline = true;
    bool role_labels = true;
    bool team_labels = true;
    bool distance_labels = true;
    bool skeleton = true;
    bool role_colors = true;
    float max_distance = (std::numeric_limits<float>::max)();
    float box_thickness = 1.5f;
    float skeleton_thickness = 2.0f;
    float head_length = 0.5f;
    std::uint32_t box_color = 0xFFFFFFFFu;
    std::uint32_t cop_color = 0xFFFF702Fu;
    std::uint32_t robber_color = 0xFF3535FFu;
    std::uint32_t distance_color = 0xFFFFFFFFu;
    std::uint32_t torso_color = 0xFFF2C259u;
    std::uint32_t legs_color = 0xFF78E17Bu;
    std::uint32_t head_color = 0xFF529BFFu;
    std::uint32_t arms_color = 0xFFEAADF1u;
    bool diagnostics = false;
    bool weapon_labels = true;
    bool show_unarmed = true;
    bool weapon_ammo = true;
    int esp_weapon_display_mode = 0; // 0 Text, 1 Icon, 2 Both.
    game_entities::EspLayout esp_layout;
};

class ESPModule final {
public:
    explicit ESPModule(bool diagnostics = true) noexcept : diagnostics_(diagnostics && scarlex::build::diagnostics) {}
    explicit ESPModule(ESPSettings settings) noexcept
        : settings_(settings), diagnostics_(settings.diagnostics && scarlex::build::diagnostics) {}

    // Call between OverlayD3D11::begin_frame() and OverlayD3D11::end_frame().
    void on_render(const game_entities::GameEntities& snapshot, const OverlayD3D11& overlay,
                   float weapon_icon_size = 48.0f) const;

private:
    ESPSettings settings_{};
    bool diagnostics_;
};
