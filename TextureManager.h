#pragma once

#include "imgui.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#include <unordered_map>

// Owned by the overlay. Use on the render thread; IDs remain valid until Clear/destruction.
class TextureManager final {
public:
    explicit TextureManager(ID3D11Device* device,
                            std::filesystem::path base_directory = ExecutableDirectory());
    ~TextureManager();
    TextureManager(const TextureManager&) = delete;
    TextureManager& operator=(const TextureManager&) = delete;

    [[nodiscard]] ImTextureID LoadTexture(const std::string& path);
    [[nodiscard]] ImTextureID WeaponIcon(const std::string& raw_id);
    [[nodiscard]] ImVec2 IconSize(ImTextureID texture, float width = 48.0f) const noexcept;
    void PreloadWeaponIcons();
    void Clear() noexcept;
    [[nodiscard]] std::size_t cached_count() const noexcept { return textures_.size(); }
    [[nodiscard]] std::size_t loaded_count() const noexcept { return sizes_.size(); }
    [[nodiscard]] static std::filesystem::path ExecutableDirectory();

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    std::filesystem::path base_directory_;
    std::unordered_map<std::string, ImTextureID> textures_; // Includes cached failures (zero).
    std::unordered_map<ImTextureID, ImVec2> sizes_;
};
