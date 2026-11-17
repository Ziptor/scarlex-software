#pragma once

#include <cstdint>
#include <optional>
#include <windows.h>

class OverlayD3D11;

namespace game_entities { class GameEntities; }

struct RadarSettings {
    float radar_size = 150.0f;   // Radius in pixels.
    float radar_scale = 1.0f;    // World meters per pixel.
    bool show_players = true;
    bool show_loot = true;
    bool enabled = true;
    bool camera_relative = true;
    bool grid = true;
    int corner = 3;             // Top-left, top-right, bottom-left, bottom-right.
    float marker_size = 3.0f;
    std::uint32_t background = 0xCC171717u;
    std::uint32_t border = 0xF0FFFFFFu;
    std::uint32_t local_color = 0xFF80E080u;
    std::uint32_t cop_color = 0xFFFF702Fu;
    std::uint32_t robber_color = 0xFF3535FFu;
    std::uint32_t neutral_color = 0xFFFFFFFFu;
    std::uint32_t loot_color = 0xFF42C8FFu;
    bool custom_position = false;
    float position_x = 1.0f;
    float position_y = 1.0f;
    bool position_in_client = false; // Older profiles used the inset work area.
};

struct RadarPlacement {
    float x, y, radius;
    float min_x, min_y, max_x, max_y;
};

class RadarModule final {
public:
    explicit RadarModule(RadarSettings settings = {}) noexcept;
    [[nodiscard]] std::optional<RadarPlacement> placement(RECT work, RECT drawable) const noexcept;
    void on_render(const game_entities::GameEntities& snapshot, const OverlayD3D11& overlay) const;

private:
    RadarSettings settings_;
};
