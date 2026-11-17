#pragma once

#include "core/types.h"
#include "game_entities.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

class OverlayD3D11;
namespace visibility { class VisibilityClient; }

namespace aimbot {

enum TargetZone { head, body, hands, legs };

struct AimbotSettings {
    bool enabled = false;
    float fov_radius = 45.0f;      // Pixels.
    float smooth = 0.5f;           // Fraction of the screen error per new snapshot.
    int aim_key = 0x02;            // VK_RBUTTON.
    float max_distance = 300.0f;   // Meters.
    float bullet_speed = 800.0f;   // Meters per second.
    bool predict = true;
    bool visible_only = false;
    int target_bone = TargetZone::head;  // Stored as an integer for existing profiles.
    float head_offset = 0.15f;     // Meters down along world Y in head mode.
    // Unknown allows all roles.
    game_entities::CharacterRole target_role = game_entities::CharacterRole::unknown;
    bool trace_every_update = false;
    bool show_fov = true;
    bool show_target_line = true;
    std::uint32_t indicator_color = 0xA0FFFFFFu;

    bool operator==(const AimbotSettings&) const = default;
};

class AimbotModule final {
public:
    explicit AimbotModule(AimbotSettings settings = {}) noexcept;
    void set_settings(AimbotSettings settings) noexcept;
    [[nodiscard]] const AimbotSettings& settings() const noexcept { return settings_; }

    void on_update(const game_entities::GameEntities& snapshot, int width, int height,
                   bool input_allowed, bool new_snapshot, const char* input_block_reason = "input disallowed",
                   visibility::VisibilityClient* visibility_client = nullptr);
    void on_render(const OverlayD3D11& overlay) const;
    void reset() noexcept;

    // Zero means SendInput failed without a Win32 error code.
    [[nodiscard]] std::optional<std::uint32_t> last_input_error() const noexcept { return last_input_error_; }

private:
    using Clock = std::chrono::steady_clock;
    [[nodiscard]] bool trace_stage(std::string_view stage, bool key_pressed);

    AimbotSettings settings_;
    std::uint64_t target_address_{};
    std::optional<core::Vec2> target_screen_;
    core::Vec2 remainder_{};
    std::optional<std::uint32_t> last_input_error_;
    std::optional<std::uint32_t> reported_input_error_;
    std::string logged_stage_;
    bool logged_key_pressed_{};
    Clock::time_point trace_after_{};
};

}  // namespace aimbot
