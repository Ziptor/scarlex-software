#pragma once
#include "release_build.h"

#include "ESPModule.h"
#include "LootModule.h"
#include "RadarModule.h"
#include "aimbot_module.h"
#include "ArrowIndicatorModule.h"

#include <array>
#include <filesystem>
#include <string>
#include <windows.h>

struct ImFont;
class TextureManager;

struct MenuSettings {
    aimbot::AimbotSettings aim;
    ESPSettings esp;
    RadarSettings radar;
    LootSettings loot;
    ArrowIndicatorSettings indicators;
    bool mesh = true;
    bool mesh_capsules = true;
    float mesh_distance = 10000.0f;
    std::uint32_t mesh_color = 0x6658BDFFu;
    bool crosshair = true;
    bool diagnostics = false;
    bool performance_log = scarlex::build::diagnostics;
    bool autosave = true;
    int menu_key = VK_INSERT;
    int exit_key = VK_END;
    float menu_opacity = 0.88f;
    std::uint32_t accent_color = 0xFF1B1BE0u;
    float weapon_icon_size = 48.0f;
    game_data::Language language = game_data::Language::russian;
};

class MenuModule final {
public:
    struct Stats {
        double read_ms{};
        double snapshot_age_ms{};
        std::size_t entities{};
        std::size_t loot{};
        std::uint64_t ioctls{};
        bool snapshot_ready{};
    };

    explicit MenuModule(std::filesystem::path settings_path);
    static std::filesystem::path default_settings_path();

    // Call on the render thread. False requests orderly application shutdown.
    [[nodiscard]] bool on_update(bool hotkeys_allowed);
    void on_render(RECT work, RECT drawable, const Stats& stats,
                   std::uint64_t logo = 0, ImFont* font = nullptr, TextureManager* textures = nullptr);
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] MenuSettings& settings() noexcept { return settings_; }
    [[nodiscard]] const std::string& status() const noexcept { return status_; }

    bool save_settings();
    bool load_settings();
    void finish();

private:
    enum class Binding { none, aim, menu, exit };
    enum class Page { aim, esp, radar, loot, indicators, settings };

    bool aim_tab();
    bool esp_tab(TextureManager* textures);
    bool layout_editor(TextureManager* textures);
    bool radar_tab(RECT work, RECT drawable);
    bool loot_tab();
    bool indicators_tab();
    bool settings_tab(const Stats& stats);
    void radar_handle(RECT work, RECT drawable);
    void key_button(const char* label, Binding binding, int key);
    void capture_key();
    void synchronize_hotkeys() noexcept;
    void close_panel();
    [[nodiscard]] const char* tr(const char* en, const char* ru) const noexcept {
        return settings_.language == game_data::Language::russian ? ru : en;
    }

    std::filesystem::path path_;
    MenuSettings settings_;
    std::string status_;
    std::array<bool, 255> last_keys_{};
    Binding binding_{};
    Page page_{};
    float radar_grab_x_{}, radar_grab_y_{};
    float radar_start_x_{}, radar_start_y_{};
    bool open_{};
    bool exiting_{};
    bool dirty_{};
    bool menu_down_{};
    bool exit_down_{};
    int layout_selected_{};
    std::array<std::array<char, 144>, 4> loot_search_{};
    bool layout_save_pending_{};
    int layout_drag_item_=-1;
    float layout_drag_x_{},layout_drag_y_{},layout_grab_x_{},layout_grab_y_{};
    bool layout_dragging_{};
    game_entities::EspAnchor layout_drop_anchor_=game_entities::EspAnchor::bottom_center;
};
