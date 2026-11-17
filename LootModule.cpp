#include "LootModule.h"

#include "OverlayD3D11.h"
#include "core/math.h"
#include "game_entities.h"
#include "ItemPresentation.h"
#include "game_data.h"

#include <cmath>
#include <cstdio>

namespace {

core::Vec3 to_world(game_entities::Vec3 p) { return {p.x, p.y, p.z}; }

struct Category {
    bool enabled;
    std::uint32_t color;
};

Category category(game_entities::LootKind kind, const LootSettings& settings) {
    using game_entities::LootKind;
    switch (kind) {
    case LootKind::bag: return {settings.bags, settings.bag_color};
    case LootKind::container: return {settings.containers, settings.container_color};
    case LootKind::cargo: return {settings.cargo, settings.cargo_color};
    case LootKind::weapon: return {settings.weapons, 0xFF3535FFu};
    case LootKind::ground_item: return {settings.ground_items, settings.ground_color};
    case LootKind::vehicle: return {settings.vehicles, settings.vehicle_color};
    }
    return {false, 0u};
}

bool passes_filter(const game_data::Match& item, const LootSettings& settings) {
    if (!item.found() || !settings.item_enabled(item)) return false;
    if (settings.min_price <= 0.0f) return true;
    const auto price = game_data::filter_price(item);
    return price && *price >= settings.min_price;
}

}  // namespace

void LootModule::on_render(const game_entities::GameEntities& snapshot, const OverlayD3D11& overlay) const {
    render(snapshot, overlay, settings_, game_data::Language::russian);
}

void LootModule::render(const game_entities::GameEntities& snapshot, const OverlayD3D11& overlay,
                        const LootSettings& settings, game_data::Language language) {
    const int width = overlay.width(), height = overlay.height();
    if (!settings.enabled || snapshot.loot_items().empty() || width <= 0 || height <= 0) {
        return;
    }
    const auto& camera = snapshot.camera();
    constexpr auto loot_clip_depth = core::ClipDepthRange::zero_to_one;
    const auto matrix = core::math::view_projection(
        {to_world(camera.position), to_world(camera.forward), to_world(camera.right), to_world(camera.up)},
        {camera.fov > 0.0f && camera.fov < 179.0f ? camera.fov : 90.0f,
         camera.near_plane, camera.far_plane, loot_clip_depth},
        static_cast<float>(width) / static_cast<float>(height));
    if (!matrix) {
        return;
    }
    for (const auto& item : snapshot.loot_items()) {
        if (!std::isfinite(item.distance) || item.distance < 0.0f || item.distance > settings.max_distance) {
            continue;
        }
        const auto style = category(item.type, settings);
        if (!style.enabled) {
            continue;
        }
        if (settings.hide_empty && item.contents && item.contents->empty()) continue;
        const auto metadata = game_data::lookup(item.name);
        if (!metadata.found()) game_data::report_unmatched(item.name, metadata, "loot");
        const bool show_item = passes_filter(metadata, settings);
        const bool show_contents = settings.show_contents && item.contents && !item.contents->empty();
        if (!show_item && !show_contents) continue;
        const auto screen = core::math::world_to_screen(to_world(item.position), *matrix, width, height, loot_clip_depth);
        if (!screen) {
            continue;
        }
        const float radius = settings.marker_size;
        const bool weapon = item.type == game_entities::LootKind::weapon || metadata.weapon ||
                            item_presentation::weapon_name(item.name);
        const auto color = weapon ? 0xFF3535FFu : game_data::rarity_color(metadata,
            item_presentation::display_color(item.rarity, false, style.color));
        float y = screen->y;
        if (show_item) {
            if (settings.markers) {
                overlay.draw_rect(screen->x - radius, y - radius, screen->x + radius, y + radius, color);
                y += radius + 2.0f;
            }
            auto label = settings.names ? game_data::display_name(metadata, weapon, language) : std::string{};
            if (settings.distance) {
                char distance[48]{};
                std::snprintf(distance, sizeof(distance), "%.0f m", item.distance);
                if (!label.empty()) label += " - ";
                label += distance;
            }
            if (!label.empty()) {
                overlay.draw_text(screen->x, y, label, color);
                y += 15.0f;
            }
            const auto extra = settings.names ? game_data::details(metadata, settings.show_price, language)
                : settings.show_price ? game_data::price_label(metadata, language) : std::string{};
            if (!extra.empty()) {
                overlay.draw_text(screen->x, y, extra, color);
                y += 15.0f;
            }
        }
        if (!show_contents) continue;
        constexpr std::size_t max_lines = 5;
        std::size_t displayed{}, remaining{};
        for (const auto& stack : *item.contents) {
            const auto stack_metadata = game_data::lookup(stack.name);
            if (!stack_metadata.found()) {
                game_data::report_unmatched(stack.name, stack_metadata, "loot-contents");
                continue;
            }
            if (!passes_filter(stack_metadata, settings)) continue;
            const auto entry = category(stack.type, settings);
            if (stack.type != game_entities::LootKind::ground_item && !entry.enabled) continue;
            if (displayed == max_lines) {
                ++remaining;
                continue;
            }
            const std::string count = stack.count ? " x" + std::to_string(stack.count) : "";
            const bool stack_weapon = stack.type == game_entities::LootKind::weapon || stack_metadata.weapon ||
                                      item_presentation::weapon_name(stack.name);
            auto line = game_data::display_name(stack_metadata, stack_weapon, language) + count;
            const auto stack_extra = game_data::details(stack_metadata, settings.show_price, language);
            if (!stack_extra.empty()) line += " · " + stack_extra;
            const auto stack_color = stack_weapon ? 0xFF3535FFu : game_data::rarity_color(stack_metadata,
                item_presentation::display_color(stack.rarity, false, entry.color));
            overlay.draw_text(screen->x, y, line, stack_color);
            ++displayed;
            y += 15.0f;
        }
        if (remaining) {
            const char* more = language == game_data::Language::russian ? " ещё" : " more";
            overlay.draw_text(screen->x, y, "+" + std::to_string(remaining) + more, color);
        }
    }
}
