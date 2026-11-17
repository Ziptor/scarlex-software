#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#include <windows.h>

namespace core { struct Vec4; struct Vec2; }
namespace game_entities { struct Vec3; }
struct ImFont;
class TextureManager;

class OverlayD3D11 final {
public:
    OverlayD3D11();
    ~OverlayD3D11();

    OverlayD3D11(const OverlayD3D11&) = delete;
    OverlayD3D11& operator=(const OverlayD3D11&) = delete;

    // All methods, including destruction, run on the window thread.
    [[nodiscard]] HRESULT initialize(HWND target);
    [[nodiscard]] HRESULT set_menu_interactive(bool interactive);
    [[nodiscard]] bool menu_has_focus() const noexcept;
    [[nodiscard]] std::uint64_t menu_logo() const noexcept;
    [[nodiscard]] ImFont* menu_font() const noexcept;
    [[nodiscard]] TextureManager* texture_manager() const noexcept;
    void set_crosshair(bool visible) noexcept;
    [[nodiscard]] bool pump_messages();
    // S_OK opens a frame. S_FALSE lets the caller pump messages and retry.
    [[nodiscard]] HRESULT begin_frame();
    [[nodiscard]] HRESULT end_frame();
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    // Visible work area in overlay client coordinates.
    [[nodiscard]] RECT visible_client_rect() const noexcept;
    // Drawable client area on the desktop, including areas outside the monitor work area.
    [[nodiscard]] RECT desktop_client_rect() const noexcept;

    // Pixel coordinates relative to the target client area. Color is 0xAABBGGRR.
    void draw_line(float x1, float y1, float x2, float y2, std::uint32_t color,
                   float thickness = 1.0f) const;
    void draw_circle(float x, float y, float radius, std::uint32_t color) const;
    void fill_circle(float x, float y, float radius, std::uint32_t color) const;
    void draw_rect(float left, float top, float right, float bottom, std::uint32_t color,
                   float thickness = 1.0f) const;
    // x is the horizontal center; y is the top of the label.
    void draw_text(float x, float y, std::string_view text, std::uint32_t color) const;
    [[nodiscard]] core::Vec2 measure_text(std::string_view text) const;
    // Screen XY in pixels and Z in [0, 1]. Clip-space submissions retain W for clipping.
    void draw_mesh(const std::vector<game_entities::Vec3>& screen_vertices,
                   const std::vector<std::uint32_t>& indices, std::uint32_t color) const;
    void draw_mesh_clip(std::span<const core::Vec4> vertices, std::span<const std::uint32_t> indices,
                        std::uint32_t color) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};
