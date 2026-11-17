#pragma once

#include <cstdint>

class OverlayD3D11;

namespace game_entities {
class GameEntities;
}

class MeshOverlayModule final {
public:
    explicit MeshOverlayModule(std::uint32_t color = 0x6658BDFFu, bool diagnostics = false,
                               bool capsules = true, float max_distance = 10000.0f) noexcept
        : color_(color), diagnostics_(diagnostics), capsules_(capsules), max_distance_(max_distance) {}

    void on_render(const game_entities::GameEntities& snapshot, const OverlayD3D11& overlay) const;

private:
    std::uint32_t color_;
    bool diagnostics_;
    bool capsules_;
    float max_distance_;
};
