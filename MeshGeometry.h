#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace game_entities {

struct MeshAffine {
    std::array<std::array<float, 4>, 3> rows{};
};

struct MeshWeight {
    float amount;
    std::uint32_t bone;
};

struct MeshGeometry {
    std::vector<std::array<float, 3>> positions;
    std::vector<std::uint32_t> indices;
    std::vector<MeshAffine> inverse_bind;
    std::vector<std::uint32_t> weight_offsets;
    std::vector<MeshWeight> weights;
};

struct MeshPose {
    std::shared_ptr<const MeshGeometry> geometry;
    // One object-to-world transform for a rigid mesh, or one world matrix per bind pose.
    std::vector<MeshAffine> bone_to_world;
};

[[nodiscard]] inline MeshAffine multiply(const MeshAffine& a, const MeshAffine& b) noexcept {
    MeshAffine result;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            result.rows[row][col] = col == 3 ? a.rows[row][3] : 0.0f;
            for (std::size_t k = 0; k < 3; ++k) {
                result.rows[row][col] += a.rows[row][k] * b.rows[k][col];
            }
        }
    }
    return result;
}

[[nodiscard]] inline std::array<float, 3> transform(const MeshAffine& matrix,
                                                   const std::array<float, 3>& point) noexcept {
    std::array<float, 3> result{};
    for (std::size_t row = 0; row < 3; ++row) {
        const auto& r = matrix.rows[row];
        result[row] = r[0] * point[0] + r[1] * point[1] + r[2] * point[2] + r[3];
    }
    return result;
}

}  // namespace game_entities
