#include "runtime_log.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "menu_module.h"

#include "imgui.h"
#include "EspLayout.h"
#include "TextureManager.h"
#include "CharacterPreview.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace aimbot {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AimbotSettings, enabled, fov_radius, smooth,
    aim_key, max_distance, bullet_speed, predict, visible_only, target_bone, head_offset, target_role,
    trace_every_update, show_fov, show_target_line, indicator_color)
}

namespace game_data {
NLOHMANN_JSON_SERIALIZE_ENUM(Language, {
    {Language::russian, "ru"},
    {Language::english, "en"},
})
}

namespace game_entities {
void to_json(nlohmann::json& j,const EspElementLayout& e) {
    j={{"anchor",esp_anchor_key(e.anchor)},{"visible",e.visible}};
}
void from_json(const nlohmann::json& j,EspElementLayout& e) {
    if(j.contains("visible")) e.visible=j.at("visible").get<bool>();
    if(const auto anchor=j.find("anchor");anchor!=j.end() && anchor->is_string()) {
        const auto key=anchor->get<std::string>();
        for(int i=0;i<4;++i) if(key==esp_anchor_key(static_cast<EspAnchor>(i))) e.anchor=static_cast<EspAnchor>(i);
    } else if(j.contains("offset_y")) {
        e.anchor=j.value("offset_y",200.0f)<100.0f ? EspAnchor::top_center : EspAnchor::bottom_center;
    }
}
void to_json(nlohmann::json& j,const EspLayout& l) {
    j={{"version",2},{"role",l.role},{"weapon",l.weapon},{"distance",l.distance},
       {"name",l.name},{"weapon_icon",l.weapon_icon}};
}
void from_json(const nlohmann::json& j,EspLayout& l) {
    l={};
    constexpr std::array keys{"role","weapon","distance","name","weapon_icon"};
    const auto fields=esp_elements(l);
    for(std::size_t i=0;i<fields.size();++i) if(const auto value=j.find(keys[i]);value!=j.end())
        from_json(*value,*fields[i]);
    if(!j.contains("weapon_icon")) if(const auto old=j.find("weapon");old!=j.end() && old->contains("offset_y")) {
        const float x=old->value("offset_x",0.0f),y=old->value("offset_y",225.0f);
        l.weapon_icon.anchor=y<120 && x<-35 ? EspAnchor::top_left : y<120 && x>35 ? EspAnchor::top_right : l.weapon.anchor;
        l.weapon_icon.visible=l.weapon.visible;
    }
}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ESPSettings, enabled, boxes, box_outline,
    role_labels, team_labels, distance_labels, skeleton, role_colors, max_distance, box_thickness,
    skeleton_thickness, head_length, box_color, cop_color, robber_color, distance_color,
    torso_color, legs_color, head_color, arms_color, diagnostics,
    weapon_labels, show_unarmed, weapon_ammo, esp_weapon_display_mode)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ArrowIndicatorSettings, enabled, arrow_radius,
    arrow_size, max_distance, show_only_offscreen, arrow_show_weapon_icon)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RadarSettings, radar_size, radar_scale,
    show_players, show_loot, enabled, camera_relative, grid, corner, marker_size,
    background, border, local_color, cop_color, robber_color, neutral_color, loot_color,
    custom_position, position_x, position_y, position_in_client)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LootSettings, enabled, bags, containers,
    cargo, weapons, ground_items, vehicles, show_contents, markers, names, distance, max_distance,
    marker_size, bag_color, container_color, cargo_color, weapon_color, ground_color, vehicle_color,
    hide_empty, show_price, min_price, disabled_items)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MenuSettings, aim, esp, radar, loot, mesh,
    mesh_capsules, mesh_distance, mesh_color, crosshair, diagnostics, performance_log,
    autosave, menu_key, exit_key, menu_opacity, accent_color, indicators, weapon_icon_size, language)

namespace {

constexpr float unlimited = (std::numeric_limits<float>::max)();

enum class LootGroup { weapons, items, containers, all };
enum class LootCategory { weapon, item, cargo, container, point };

std::wstring folded_text(std::string_view text) {
    if (text.empty()) return {};
    const auto length = static_cast<int>(text.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (!count) return {};
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, wide.data(), count)) return {};
    const int folded_count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(), count,
                                           nullptr, 0, nullptr, nullptr, 0);
    if (folded_count <= 0) return wide;
    std::wstring folded(static_cast<std::size_t>(folded_count), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(), count, folded.data(),
                       folded_count, nullptr, nullptr, 0)) return wide;
    return folded;
}

struct LootCatalogRow {
    game_data::Match item;
    LootGroup group{};
    LootCategory category{};
    int rarity{};
    std::array<std::string, 2> names, prices;
    std::array<std::wstring, 2> folded_names;
    std::wstring folded_id;
};

const std::vector<LootCatalogRow>& loot_catalog() {
    static const auto rows = [] {
        using namespace game_data;
        const auto& catalog_data = catalog();
        std::vector<LootCatalogRow> entries;
        entries.reserve(catalog_data.items.size() + catalog_data.weapons.size() + catalog_data.map_points.size());
        const auto add = [&](const auto& values) {
            for (const auto& [key, unused] : values) {
                auto match = lookup(key);
                if (match.normalized_id != key) continue;
                const bool weapon = match.weapon || (match.item && match.item->category == "Оружие");
                LootCatalogRow row;
                if (weapon) row.category = LootCategory::weapon;
                else if (match.point) row.category = LootCategory::point;
                else if (match.item && match.item->category == "Грузы") row.category = LootCategory::cargo;
                else if (match.item && match.item->category == "Контейнеры") row.category = LootCategory::container;
                else row.category = LootCategory::item;
                row.group = weapon ? LootGroup::weapons : row.category == LootCategory::item
                    ? LootGroup::items : LootGroup::containers;
                row.rarity = rarity_value(match);
                row.folded_id = folded_text(key);
                for (const auto language : {Language::english, Language::russian}) {
                    const auto i = static_cast<std::size_t>(language);
                    row.names[i] = display_name(match, weapon, language);
                    row.prices[i] = price_label(match, language);
                    row.folded_names[i] = folded_text(row.names[i]);
                }
                row.item = std::move(match);
                entries.push_back(std::move(row));
            }
        };
        add(catalog_data.items);
        add(catalog_data.weapons);
        add(catalog_data.map_points);
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.item.normalized_id < b.item.normalized_id;
        });
        entries.erase(std::unique(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.item.normalized_id == b.item.normalized_id;
        }), entries.end());
        return entries;
    }();
    return rows;
}

void section_heading(const char* title, ImU32 accent) {
    const auto pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled({pos.x, pos.y + 3.0f}, {pos.x + 3.0f, pos.y + 13.0f}, accent, 1.0f);
    ImGui::Indent(12.0f);
    ImGui::TextDisabled("%s", title);
    ImGui::Unindent(12.0f);
    ImGui::Separator();
}

template <class Body>
void settings_card(const char* id, const char* title, ImU32 accent, Body&& body) {
    if (ImGui::BeginChild(id, {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoScrollbar)) {
        section_heading(title, accent);
        body();
    }
    ImGui::EndChild();
}

template <class Left, class Right>
void settings_columns(float min_width, Left&& left, Right&& right) {
    if (ImGui::GetContentRegionAvail().x >= min_width &&
        ImGui::BeginTable("SettingsColumns", 2, ImGuiTableFlags_SizingStretchSame |
                         ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Left");
        ImGui::TableSetupColumn("Right");
        ImGui::TableNextColumn();
        left();
        ImGui::TableNextColumn();
        right();
        ImGui::EndTable();
    } else {
        left();
        right();
    }
}

bool setting_toggle(const char* label, bool& value) {
    const float width = (std::max)(70.0f, ImGui::GetContentRegionAvail().x);
    const float wrap_width = width - 34.0f;
    const float text_height = ImGui::CalcTextSize(label, nullptr, false, wrap_width).y;
    const float height = (std::max)(30.0f, text_height + 10.0f);
    const auto pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, {width, height}, ImGuiButtonFlags_EnableNav);
    if (pressed) value = !value;
    const auto accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    auto* draw = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered()) {
        draw->AddRectFilled(pos, {pos.x + width, pos.y + height}, IM_COL32(255, 255, 255, 10), 5.0f);
    }
    const float x = pos.x + width - 23.0f;
    const float y = pos.y + (height - 20.0f) * 0.5f;
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {pos.x + 2, pos.y + (height - text_height) * 0.5f},
                  ImGui::GetColorU32(ImGuiCol_Text), label, nullptr, wrap_width);
    draw->AddRectFilled({x, y}, {x + 20, y + 20}, value ? accent : IM_COL32(28, 28, 31, 245), 5.0f);
    if (value) {
        const auto white = IM_COL32(255, 255, 255, 255);
        draw->AddLine({x + 5, y + 10}, {x + 9, y + 14}, white, 1.7f);
        draw->AddLine({x + 9, y + 14}, {x + 15, y + 6}, white, 1.7f);
    } else {
        draw->AddRect({x, y}, {x + 20, y + 20}, IM_COL32(67, 67, 73, 235), 5.0f);
    }
    return pressed;
}

bool language_button(game_data::Language language) {
    const bool russian = language == game_data::Language::russian;
    const auto pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##MenuLanguage", {38.0f, 22.0f});
    auto* draw = ImGui::GetWindowDrawList();
    const float x = pos.x + 3, y = pos.y + 2, w = 32, h = 18;
    if (ImGui::IsItemHovered()) {
        draw->AddRectFilled(pos, {pos.x + 38, pos.y + 22}, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    }
    if (russian) {
        draw->AddRectFilled({x,y}, {x+w,y+6}, IM_COL32(255,255,255,255));
        draw->AddRectFilled({x,y+6}, {x+w,y+12}, IM_COL32(0,57,166,255));
        draw->AddRectFilled({x,y+12}, {x+w,y+h}, IM_COL32(213,43,30,255));
    } else {
        const auto white = IM_COL32(255,255,255,255);
        const auto red = IM_COL32(207,20,43,255);
        draw->AddRectFilled({x,y}, {x+w,y+h}, IM_COL32(1,33,105,255));
        draw->PushClipRect({x,y}, {x+w,y+h}, true);
        for (const float dy : {0.0f, h}) {
            draw->AddLine({x,y+dy}, {x+w,y+h-dy}, white, 4.0f);
            draw->AddLine({x,y+dy}, {x+w,y+h-dy}, red, 1.4f);
        }
        draw->AddRectFilled({x+w/2-3,y}, {x+w/2+3,y+h}, white);
        draw->AddRectFilled({x,y+h/2-3}, {x+w,y+h/2+3}, white);
        draw->AddRectFilled({x+w/2-1.7f,y}, {x+w/2+1.7f,y+h}, red);
        draw->AddRectFilled({x,y+h/2-1.7f}, {x+w,y+h/2+1.7f}, red);
        draw->PopClipRect();
    }
    draw->AddRect({x,y}, {x+w,y+h}, IM_COL32(135,135,135,255));
    ImGui::SetItemTooltip("%s", russian ? "Switch to English" : "Переключить на русский");
    return pressed;
}

void navigation_icon(ImDrawList& draw, int page, ImVec2 center, ImU32 color) {
    const auto p = [&](float x, float y) { return ImVec2{center.x + x, center.y + y}; };
    switch (page) {
    case 0:
        draw.AddCircle(center, 6, color, 22, 1.4f);
        draw.AddLine(p(-9,0), p(-4,0), color);
        draw.AddLine(p(4,0), p(9,0), color);
        draw.AddLine(p(0,-9), p(0,-4), color);
        draw.AddLine(p(0,4), p(0,9), color);
        break;
    case 1: {
        const ImVec2 eye[]{p(-9,0), p(0,-6), p(9,0), p(0,6)};
        draw.AddPolyline(eye, 4, color, ImDrawFlags_Closed, 1.4f);
        draw.AddCircleFilled(center, 2.4f, color, 14);
        break;
    }
    case 2:
        draw.AddCircle(center, 8, color, 24, 1.3f);
        draw.AddCircle(center, 4.5f, color, 20, 1.0f);
        draw.AddLine(center, p(7,-7), color, 1.4f);
        draw.AddCircleFilled(p(-3,3), 1.4f, color);
        break;
    case 3: {
        const ImVec2 box[]{p(0,-8), p(8,-4), p(8,4), p(0,8), p(-8,4), p(-8,-4)};
        const ImVec2 fold[]{p(-8,-4), p(0,0), p(8,-4)};
        draw.AddPolyline(box, 6, color, ImDrawFlags_Closed, 1.3f);
        draw.AddPolyline(fold, 3, color, 0, 1.3f);
        draw.AddLine(p(0,0), p(0,8), color, 1.3f);
        break;
    }
    case 4:
        draw.AddTriangle(p(0,-8), p(8,6), p(-8,6), color, 1.5f);
        draw.AddLine(p(0,-3), p(0,1), color, 1.4f);
        draw.AddCircleFilled(p(0,3.5f), 0.8f, color);
        break;
    case 5:
        for (int i = 0; i < 3; ++i) {
            const float y = static_cast<float>(i - 1) * 5;
            draw.AddLine(p(-8,y), p(8,y), color, 1.2f);
            draw.AddCircleFilled(p(i == 1 ? 3.0f : -3.0f,y), 2.2f, color, 12);
        }
        break;
    }
}

bool navigation_button(const char* label, int page, bool selected, ImVec4 accent) {
    const auto text_color = ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{});
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4{accent.x * 0.21f, accent.y * 0.21f,
                                                            accent.z * 0.21f, 0.92f} : ImVec4{});
    const bool pressed = ImGui::Button(label, {-1.0f, 41.0f});
    ImGui::PopStyleColor(2);
    const auto a = ImGui::GetItemRectMin();
    const auto b = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    const float cy = (a.y + b.y) * 0.5f;
    if (selected) {
        const auto color = ImGui::GetColorU32(accent);
        draw->AddRect(a, b, ImGui::GetColorU32({accent.x, accent.y, accent.z, 0.50f}), 7.0f);
        draw->AddRectFilled({a.x, cy - 11}, {a.x + 3, cy + 11}, color, 2.0f);
    }
    const ImVec2 text{a.x + 39, cy - ImGui::GetTextLineHeight() * 0.5f};
    draw->PushClipRect(a, b, true);
    navigation_icon(*draw, page, {a.x + 20, cy}, selected ? ImGui::GetColorU32(accent) : text_color);
    draw->AddText(text, text_color, label);
    draw->PopClipRect();
    return pressed;
}

bool close_menu_button(const char* tooltip) {
    const bool pressed = ImGui::InvisibleButton("##ClosePanel", {31, 30}, ImGuiButtonFlags_EnableNav);
    const auto a = ImGui::GetItemRectMin();
    const auto b = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) draw->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 6.0f);
    const ImVec2 center{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    const auto color = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    draw->AddLine({center.x - 4, center.y - 4}, {center.x + 4, center.y + 4}, color, 1.5f);
    draw->AddLine({center.x - 4, center.y + 4}, {center.x + 4, center.y - 4}, color, 1.5f);
    ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

bool key_down(int key) noexcept {
    return key > 0 && key < 255 && GetAsyncKeyState(key) < 0;
}

bool mouse_key(int key) noexcept {
    return key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON ||
           key == VK_XBUTTON1 || key == VK_XBUTTON2;
}

bool bindable_key(int key, bool allow_mouse) noexcept {
    if (mouse_key(key)) return allow_mouse;
    if (key <= VK_CANCEL || key >= 255 || key == VK_ESCAPE || key == VK_LWIN ||
        key == VK_RWIN || key == VK_MENU || key == VK_CONTROL || key == VK_SHIFT ||
        (key >= VK_LSHIFT && key <= VK_RMENU)) {
        return false;
    }
    return MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC_EX) != 0;
}

std::string key_name(int key) {
    switch (key) {
    case VK_INSERT: return "Insert";
    case VK_END: return "End";
    case VK_LBUTTON: return "Mouse 1";
    case VK_RBUTTON: return "Mouse 2";
    case VK_MBUTTON: return "Mouse 3";
    case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5";
    default: break;
    }
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC_EX);
    const LONG key_bits = static_cast<LONG>(((scan & 0xFFu) << 16) | ((scan & 0xFF00u) ? 1u << 24 : 0u));
    wchar_t name[64]{};
    const int length = GetKeyNameTextW(key_bits, name, static_cast<int>(std::size(name)));
    char utf8[256]{};
    if (length > 0 && WideCharToMultiByte(CP_UTF8, 0, name, length, utf8, sizeof(utf8), nullptr, nullptr)) {
        return utf8;
    }
    return "VK " + std::to_string(key);
}

bool slider(const char* label, float& value, float low, float high, const char* format,
            ImGuiSliderFlags flags = ImGuiSliderFlags_None) {
    ImGui::TextWrapped("%s", label);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.0f, 3.0f});
    const bool changed = ImGui::SliderFloat("##Value", &value, low, high, format,
                                          flags | ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

bool color(const char* label, std::uint32_t& packed, bool alpha = true) {
    auto rgba = ImGui::ColorConvertU32ToFloat4(packed);
    const auto options = ImGuiColorEditFlags_NoInputs |
        (alpha ? ImGuiColorEditFlags_AlphaBar : ImGuiColorEditFlags_NoAlpha);
    if (!ImGui::ColorEdit4(label, &rgba.x, options)) {
        return false;
    }
    packed = ImGui::ColorConvertFloat4ToU32(rgba);
    return true;
}

bool distance_limit(float& meters, bool russian) {
    bool limited = meters != unlimited;
    bool edited = ImGui::Checkbox(russian ? "Ограничить дистанцию" : "Limit distance", &limited);
    if (edited) meters = limited ? 300.0f : unlimited;
    if (limited) edited |= slider(russian ? "Макс. дистанция" : "Maximum distance", meters, 1.0f, 10000.0f, "%.0f m");
    return edited;
}

float position_fraction(float point, float first, float last) noexcept {
    return last > first ? std::clamp((point - first) / (last - first), 0.0f, 1.0f) : 0.0f;
}

float dragged_fraction(float pointer, float grab, float start, float first, float last) noexcept {
    grab = std::clamp(grab, first, last);
    start = std::clamp(start, first, last);
    if (pointer == grab) return position_fraction(start, first, last);
    // Map each side of the initial grab separately so the offset of the handle
    // never makes an edge unreachable.
    const float point = pointer <= grab
        ? std::lerp(first, start, position_fraction(pointer, first, grab))
        : std::lerp(start, last, position_fraction(pointer, grab, last));
    return position_fraction(point, first, last);
}

bool target_preview(int& zone, ImVec4 accent, bool russian) {
    const float available = ImGui::GetContentRegionAvail().x;
    const float scale = std::clamp(available / 172.0f, 0.55f, 0.88f);
    const ImVec2 size{172.0f * scale, 294.0f * scale};
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.0f, (available - size.x) * 0.5f));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##target-preview", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    auto* draw = ImGui::GetWindowDrawList();
    const auto at = [&](float x, float y) { return ImVec2{origin.x + x, origin.y + y}; };
    draw->AddRectFilled(origin, at(size.x, size.y), ImGui::GetColorU32(ImGuiCol_FrameBg), 6.0f);
    for (float line = 14.0f; line < size.x; line += 18.0f) {
        draw->AddLine(at(line, 8), at(line, size.y - 8), IM_COL32(144, 145, 156, 23));
    }
    for (float line = 18.0f; line < size.y; line += 18.0f) {
        draw->AddLine(at(8, line), at(size.x - 8, line), IM_COL32(144, 145, 156, 23));
    }
    draw_character_preview(draw, origin, scale);
    using aimbot::TargetZone;
    struct Marker { float x, y; TargetZone target; };
    constexpr Marker markers[]{{86, 35, TargetZone::head}, {86, 99, TargetZone::body},
        {29, 153, TargetZone::hands}, {143, 153, TargetZone::hands},
        {66, 207, TargetZone::legs}, {106, 207, TargetZone::legs}};
    bool edited = false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (const auto& marker : markers) {
        const ImVec2 position = at(marker.x * scale, marker.y * scale);
        const float x = mouse.x - position.x, y = mouse.y - position.y;
        const bool over_marker = hovered && x * x + y * y <= 14.0f * 14.0f;
        if (over_marker && clicked && zone != marker.target) {
            zone = marker.target;
            edited = true;
        }
        const bool selected = zone == marker.target;
        if (selected || over_marker) {
            draw->AddCircleFilled(position, 11.0f, ImGui::GetColorU32({accent.x, accent.y, accent.z, 0.22f}));
            draw->AddCircle(position, 8.0f, ImGui::GetColorU32(accent));
        }
        draw->AddCircleFilled(position, 4.0f, selected || over_marker ? ImGui::GetColorU32(accent)
                                                              : IM_COL32(158, 157, 168, 255));
    }
    constexpr const char* names[]{"Head", "Body", "Hands", "Legs"};
    constexpr const char* names_ru[]{"Голова", "Тело", "Руки", "Ноги"};
    ImGui::TextColored(accent, "%s", russian ? names_ru[zone] : names[zone]);
    ImGui::TextDisabled("%s", russian ? "Нажмите на точку" : "Click a point to select");
    return edited;
}

float bounded(float value, float minimum, float maximum, float fallback) {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

void validate(MenuSettings& in) {
    if constexpr (!scarlex::build::diagnostics) {
        in.diagnostics = in.performance_log = in.esp.diagnostics = in.aim.trace_every_update = false;
    }
    const MenuSettings defaults;
    if (!bindable_key(in.menu_key, false) || !bindable_key(in.exit_key, false) ||
        !bindable_key(in.aim.aim_key, true) || in.menu_key == in.exit_key ||
        in.menu_key == in.aim.aim_key || in.exit_key == in.aim.aim_key) {
        throw std::runtime_error("Invalid or conflicting key bindings.");
    }
    auto& aim = in.aim;
    aim.fov_radius = bounded(aim.fov_radius, 0.0f, 16384.0f, defaults.aim.fov_radius);
    aim.smooth = bounded(aim.smooth, 0.1f, 1.0f, defaults.aim.smooth);
    aim.max_distance = bounded(aim.max_distance, 1.0f, 10000.0f, defaults.aim.max_distance);
    aim.bullet_speed = bounded(aim.bullet_speed, 1.0f, 20000.0f, defaults.aim.bullet_speed);
    aim.head_offset = bounded(aim.head_offset, 0.0f, 2.0f, defaults.aim.head_offset);
    if (aim.target_bone < aimbot::TargetZone::head || aim.target_bone > aimbot::TargetZone::legs) {
        aim.target_bone = aimbot::TargetZone::head;
    }
    using game_entities::CharacterRole;
    if (aim.target_role > CharacterRole::robber) aim.target_role = CharacterRole::unknown;

    auto& esp = in.esp;
    if (esp.max_distance != unlimited) {
        esp.max_distance = bounded(esp.max_distance, 1.0f, 10000.0f, unlimited);
    }
    esp.box_thickness = bounded(esp.box_thickness, 0.5f, 8.0f, defaults.esp.box_thickness);
    esp.skeleton_thickness = bounded(esp.skeleton_thickness, 0.5f, 8.0f, defaults.esp.skeleton_thickness);
    esp.head_length = bounded(esp.head_length, 0.0f, 1.0f, defaults.esp.head_length);
    esp.esp_weapon_display_mode = std::clamp(esp.esp_weapon_display_mode,0,2);
    in.weapon_icon_size = bounded(in.weapon_icon_size,16.0f,64.0f,defaults.weapon_icon_size);
    const auto elements=game_entities::esp_elements(esp.esp_layout);
    for (std::size_t i=0;i<elements.size();++i) {
        elements[i]->anchor=game_entities::valid_esp_anchor(elements[i]->anchor,i==4);
    }

    auto& radar = in.radar;
    radar.radar_size = bounded(radar.radar_size, 12.0f, 500.0f, defaults.radar.radar_size);
    radar.radar_scale = bounded(radar.radar_scale, 0.05f, 20.0f, defaults.radar.radar_scale);
    radar.marker_size = bounded(radar.marker_size, 1.0f, 12.0f, defaults.radar.marker_size);
    radar.corner = std::clamp(radar.corner, 0, 3);
    radar.position_x = bounded(radar.position_x, 0.0f, 1.0f, defaults.radar.position_x);
    radar.position_y = bounded(radar.position_y, 0.0f, 1.0f, defaults.radar.position_y);

    auto& loot = in.loot;
    loot.max_distance = bounded(loot.max_distance, 1.0f, 10000.0f, defaults.loot.max_distance);
    loot.min_price = bounded(loot.min_price, 0.0f, 2'000'000.0f, defaults.loot.min_price);
    loot.marker_size = bounded(loot.marker_size, 1.0f, 15.0f, defaults.loot.marker_size);
    in.mesh_distance = bounded(in.mesh_distance, 1.0f, 10000.0f, defaults.mesh_distance);
    in.menu_opacity = bounded(in.menu_opacity, 0.30f, 1.0f, defaults.menu_opacity);
    in.indicators.arrow_radius = bounded(in.indicators.arrow_radius, 32.0f, 1000.0f, 80.0f);
    in.indicators.arrow_size = bounded(in.indicators.arrow_size, 4.0f, 24.0f, 10.0f);
    in.indicators.max_distance = bounded(in.indicators.max_distance, 1.0f, 10000.0f, 200.0f);
    if (in.language != game_data::Language::english && in.language != game_data::Language::russian) {
        in.language = defaults.language;
    }
}

}  // namespace

MenuModule::MenuModule(std::filesystem::path settings_path) : path_(std::move(settings_path)) {
    std::error_code error;
    if (std::filesystem::exists(path_, error)) {
        load_settings();
    } else if (error) {
        status_ = "Could not check settings file: " + error.message();
    }
    synchronize_hotkeys();
}

std::filesystem::path MenuModule::default_settings_path() {
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (length != 0) {
        std::wstring directory(length, L'\0');
        const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", directory.data(), length);
        if (copied > 0 && copied < length) {
            directory.resize(copied);
            return std::filesystem::path(directory) / L"RobRunTool" / L"menu_settings.json";
        }
    }
    return std::filesystem::current_path() / L"menu_settings.json";
}

void MenuModule::synchronize_hotkeys() noexcept {
    menu_down_ = key_down(settings_.menu_key);
    exit_down_ = key_down(settings_.exit_key);
}

bool MenuModule::on_update(bool hotkeys_allowed) {
    const bool menu_was_down = menu_down_;
    const bool exit_was_down = exit_down_;
    synchronize_hotkeys();
    if (!hotkeys_allowed) return !exiting_;
    if (binding_ != Binding::none) {
        capture_key();
    } else if (exit_down_ && !exit_was_down) {
        exiting_ = true;
    } else if (menu_down_ && !menu_was_down) {
        if (open_) close_panel();
        else open_ = true;
    }
    return !exiting_;
}

void MenuModule::capture_key() {
    const bool cancel = key_down(VK_ESCAPE);
    if (cancel && !last_keys_[VK_ESCAPE]) {
        binding_ = Binding::none;
        status_ = tr("Binding canceled.", "Привязка отменена.");
        return;
    }
    last_keys_[VK_ESCAPE] = cancel;
    for (int key = 1; key < static_cast<int>(last_keys_.size()); ++key) {
        if (!bindable_key(key, binding_ == Binding::aim)) continue;
        const bool down = key_down(key);
        const bool pressed = down && !last_keys_[key];
        last_keys_[key] = down;
        if (!pressed) continue;
        if ((binding_ != Binding::aim && key == settings_.aim.aim_key) ||
            (binding_ != Binding::menu && key == settings_.menu_key) ||
            (binding_ != Binding::exit && key == settings_.exit_key)) {
            status_ = tr("That key is already bound. Press another key or Escape.", "Клавиша уже занята. Выберите другую или нажмите Escape.");
            continue;
        }
        switch (binding_) {
        case Binding::aim: settings_.aim.aim_key = key; break;
        case Binding::menu: settings_.menu_key = key; break;
        case Binding::exit: settings_.exit_key = key; break;
        default: break;
        }
        binding_ = Binding::none;
        dirty_ = true;
        status_ = tr("Bound to ", "Назначена клавиша ") + key_name(key) + ".";
        synchronize_hotkeys();
        return;
    }
}

void MenuModule::key_button(const char* label, Binding binding, int key) {
    ImGui::PushID(label);
    const std::string caption = binding_ == binding ? tr("Press a key...", "Нажмите клавишу…") : key_name(key);
    ImGui::TextWrapped("%s", label);
    if (ImGui::Button((caption + "###KeyCapture").c_str(), {-1.0f, 0.0f})) {
        binding_ = binding;
        for (int i = 1; i < static_cast<int>(last_keys_.size()); ++i) {
            last_keys_[i] = key_down(i);
        }
        status_ = binding == Binding::aim ? tr("Press a key or mouse button. Escape cancels.", "Нажмите клавишу или кнопку мыши. Escape отменяет выбор.")
                                          : tr("Press a keyboard key. Escape cancels.", "Нажмите клавишу. Escape отменяет выбор.");
    }
    ImGui::PopID();
}

bool MenuModule::aim_tab() {
    auto& aim = settings_.aim;
    const auto accent = settings_.accent_color;
    bool edited = false;
    settings_columns(510.0f, [&] {
        settings_card("AimControl", tr("AIM", "ПРИЦЕЛИВАНИЕ"), accent, [&] {
            edited |= setting_toggle(tr("Enable aimbot", "Включить аимбот"), aim.enabled);
            key_button(tr("Hold to aim", "Клавиша удержания"), Binding::aim, aim.aim_key);
            edited |= slider(tr("FOV radius", "Радиус FOV"), aim.fov_radius, 0.0f, 16384.0f, "%.0f px", ImGuiSliderFlags_Logarithmic);
            edited |= slider(tr("Smoothing", "Плавность"), aim.smooth, 0.1f, 1.0f, "%.2f");
            edited |= slider(tr("Maximum distance", "Макс. дистанция"), aim.max_distance, 1.0f, 10000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
            edited |= setting_toggle(tr("Lead moving targets", "Упреждать движение"), aim.predict);
            if (aim.predict) {
                edited |= slider(tr("Projectile speed", "Скорость пули"), aim.bullet_speed,
                                 1.0f, 20000.0f, "%.0f m/s", ImGuiSliderFlags_Logarithmic);
            }
        });
        settings_card("AimIndicators", tr("INDICATORS", "ИНДИКАЦИЯ"), accent, [&] {
            edited |= setting_toggle(tr("FOV circle", "Круг FOV"), aim.show_fov);
            edited |= setting_toggle(tr("Line to selected target", "Линия к цели"), aim.show_target_line);
            edited |= color(tr("Indicator color", "Цвет индикаторов"), aim.indicator_color);
            if constexpr (scarlex::build::diagnostics)
                edited |= setting_toggle(tr("Trace every aim update", "Логировать обновления"), aim.trace_every_update);
        });
    }, [&] {
        settings_card("AimZone", tr("TARGET ZONE", "ЗОНА ПРИЦЕЛИВАНИЯ"), accent, [&] {
            ImGui::SetNextItemWidth(-1.0f);
            edited |= ImGui::Combo("##AimTargetZone", &aim.target_bone,
                tr("Head\0Body\0Hands\0Legs\0", "Голова\0Тело\0Руки\0Ноги\0"));
            edited |= target_preview(aim.target_bone, ImGui::ColorConvertU32ToFloat4(accent),
                                     settings_.language == game_data::Language::russian);
            if (aim.target_bone == 0) {
                edited |= slider(tr("Head offset down", "Смещение по голове вниз"), aim.head_offset, 0.0f, 2.0f, "%.2f m");
            }
        });
        settings_card("AimFilters", tr("TARGET FILTERS", "ФИЛЬТРЫ ЦЕЛЕЙ"), accent, [&] {
            int role = static_cast<int>(aim.target_role);
            ImGui::TextUnformatted(tr("Target roles", "Роли целей"));
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##AimRoles", &role, tr("All roles\0Neutral\0Cops\0Robbers\0", "Все роли\0Мирные\0Копы\0Преступники\0"))) {
                aim.target_role = static_cast<game_entities::CharacterRole>(role);
                edited = true;
            }
            edited |= setting_toggle(tr("Visible targets only", "Только видимые цели"), aim.visible_only);
            ImGui::SetItemTooltip("%s", tr("Requires a live Unity physics provider.",
                                           "Требуется подключённый источник физики Unity."));
        });
    });
    return edited;
}

bool MenuModule::esp_tab(TextureManager* textures) {
    auto& esp = settings_.esp;
    const auto accent = settings_.accent_color;
    bool edited = false;
    settings_columns(560.0f, [&] {
        settings_card("EspLabels", tr("PLAYER LABELS", "ПОДПИСИ ИГРОКОВ"), accent, [&] {
            edited |= setting_toggle(tr("Enable player ESP", "Включить ESP игроков"), esp.enabled);
            auto& name = esp.esp_layout.name;
            if (setting_toggle(tr("Player names", "Ники игроков"), name.visible)) {
                edited = layout_save_pending_ = true;
            }
            int name_position = static_cast<int>(name.anchor);
            ImGui::SetNextItemWidth(155.0f);
            if (ImGui::Combo(tr("Name position", "Позиция ника"), &name_position, tr("Top center\0Bottom center\0", "Сверху по центру\0Снизу по центру\0"))) {
                name.anchor = static_cast<game_entities::EspAnchor>(name_position);
                edited = layout_save_pending_ = true;
            }
            edited |= distance_limit(esp.max_distance, settings_.language == game_data::Language::russian);
            edited |= setting_toggle(tr("Boxes", "Боксы"), esp.boxes);
            edited |= setting_toggle(tr("Outline", "Обводка"), esp.box_outline);
            if (setting_toggle(tr("Role labels", "Роли"), esp.role_labels)) {
                esp.esp_layout.role.visible=esp.role_labels; edited=layout_save_pending_=true;
            }
            if (setting_toggle(tr("Distance labels", "Дистанция"), esp.distance_labels)) {
                esp.esp_layout.distance.visible=esp.distance_labels; edited=layout_save_pending_=true;
            }
            edited |= setting_toggle(tr("Team / clan label", "Команда / клан"), esp.team_labels);
            if (setting_toggle(tr("Held weapon", "Оружие в руках"), esp.weapon_labels)) {
                esp.esp_layout.weapon.visible=esp.weapon_labels; edited=layout_save_pending_=true;
            }
            if (esp.weapon_labels) {
                edited |= ImGui::Combo(tr("Weapon Display", "Отображение оружия"),&esp.esp_weapon_display_mode,tr("Text\0Icon\0Both\0", "Текст\0Иконка\0Оба\0"));
                edited |= setting_toggle(tr("Magazine / reserve ammo when available", "Патроны в магазине и запасе"), esp.weapon_ammo);
                edited |= setting_toggle(tr("Show Unarmed", "Подпись без оружия"), esp.show_unarmed);
            }
            edited |= slider(tr("Box thickness", "Толщина бокса"), esp.box_thickness, 0.5f, 8.0f, "%.1f px");
            edited |= setting_toggle(tr("Color boxes by role", "Цвет бокса по роли"), esp.role_colors);
            edited |= color(tr("Default / neutral", "Мирные / по умолчанию"), esp.box_color);
            if (esp.role_colors) {
                edited |= color(tr("Cop boxes", "Копы"), esp.cop_color);
                edited |= color(tr("Robber boxes", "Преступники"), esp.robber_color);
            }
            if (esp.distance_labels) edited |= color(tr("Distance text", "Цвет дистанции"), esp.distance_color);
        });
        settings_card("EspSkeleton", tr("SKELETON", "СКЕЛЕТ"), accent, [&] {
            edited |= setting_toggle(tr("Draw skeleton", "Рисовать скелет"), esp.skeleton);
            if (esp.skeleton) {
                edited |= slider(tr("Skeleton thickness", "Толщина скелета"), esp.skeleton_thickness, 0.5f, 8.0f, "%.1f px");
                edited |= slider(tr("Head segment length", "Длина сегмента головы"), esp.head_length, 0.0f, 1.0f, "%.2f");
                edited |= color(tr("Torso", "Торс"), esp.torso_color);
                edited |= color(tr("Arms", "Руки"), esp.arms_color);
                edited |= color(tr("Legs", "Ноги"), esp.legs_color);
                edited |= color(tr("Head", "Голова"), esp.head_color);
            }
        });
    }, [&] {
        settings_card("EspLayoutCard", tr("LAYOUT EDITOR", "КОМПОНОВКА ESP"), accent, [&] {
            edited |= layout_editor(textures);
        });
        settings_card("EspMesh", tr("MODEL FILL", "ЗАЛИВКА МОДЕЛЕЙ"), accent, [&] {
            edited |= setting_toggle(tr("Draw 3D meshes", "Рисовать 3D-модели"), settings_.mesh);
            if (settings_.mesh) {
                edited |= setting_toggle(tr("Capsule fallback", "Капсулы при отсутствии модели"), settings_.mesh_capsules);
                edited |= slider(tr("Chams distance", "Дальность заливки"), settings_.mesh_distance, 1.0f, 10000.0f, "%.0f m",
                                 ImGuiSliderFlags_Logarithmic);
                edited |= color(tr("Fill color and opacity", "Цвет и прозрачность заливки"), settings_.mesh_color);
            }
            if constexpr (scarlex::build::diagnostics)
                edited |= setting_toggle(tr("Log ESP projection", "Логировать проекцию ESP"), esp.diagnostics);
        });
    });
    return edited;
}

#include "EspSnapEditor.inl"

bool MenuModule::radar_tab(RECT work, RECT drawable) {
    auto& radar = settings_.radar;
    const auto accent = settings_.accent_color;
    bool edited = false;
    settings_columns(490.0f, [&] {
        settings_card("RadarDisplay", tr("MINIMAP", "МИНИКАРТА"), accent, [&] {
            edited |= setting_toggle(tr("Enable radar", "Включить радар"), radar.enabled);
            edited |= setting_toggle(tr("Players", "Игроки"), radar.show_players);
            edited |= setting_toggle(tr("Loot markers", "Маркеры лута"), radar.show_loot);
            edited |= setting_toggle(tr("Rotate with camera", "Вращать по камере"), radar.camera_relative);
            edited |= setting_toggle(tr("Range grid", "Сетка дальности"), radar.grid);
            edited |= slider(tr("Radius", "Радиус"), radar.radar_size, 12.0f, 500.0f, "%.0f px");
            edited |= slider(tr("Scale", "Масштаб"), radar.radar_scale, 0.05f, 20.0f, "%.2f m/px", ImGuiSliderFlags_Logarithmic);
            ImGui::TextDisabled(tr("Range: %.0f m", "Дальность: %.0f м"), radar.radar_size * radar.radar_scale);
        });
    }, [&] {
        settings_card("RadarPosition", tr("POSITION", "ПОЛОЖЕНИЕ"), accent, [&] {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##RadarCorner", &radar.corner, tr("Top left\0Top right\0Bottom left\0Bottom right\0",
                    "Слева сверху\0Справа сверху\0Слева снизу\0Справа снизу\0"))) {
                radar.custom_position = false;
                edited = true;
            }
            if (radar.custom_position && ImGui::Button(tr("Reset dragged position", "Сбросить положение"))) {
                radar.custom_position = false;
                edited = true;
            }
            ImGui::TextWrapped("%s", tr("Drag the handle on the radar to place it.", "Перетащите радар за ручку."));
            if (const auto area = RadarModule(radar).placement(work, drawable)) {
                float x = position_fraction(area->x, area->min_x, area->max_x);
                float y = position_fraction(area->y, area->min_y, area->max_y);
                bool moved = false;
                if (radar.custom_position) {
                    moved |= slider(tr("Horizontal position", "По горизонтали"), x, 0.0f, 1.0f, "%.2f");
                    moved |= slider(tr("Vertical position", "По вертикали"), y, 0.0f, 1.0f, "%.2f");
                } else {
                    moved = ImGui::Button(tr("Set custom position", "Задать положение"));
                }
                if (moved) {
                    radar.position_x = x;
                    radar.position_y = y;
                    radar.custom_position = radar.position_in_client = true;
                    edited = true;
                }
            }
        });
        settings_card("RadarAppearance", tr("APPEARANCE", "ОФОРМЛЕНИЕ"), accent, [&] {
            edited |= slider(tr("Marker size", "Размер маркера"), radar.marker_size, 1.0f, 12.0f, "%.1f px");
            edited |= color(tr("Background", "Фон"), radar.background);
            edited |= color(tr("Border", "Рамка"), radar.border);
            edited |= color(tr("Local player", "Локальный игрок"), radar.local_color);
            edited |= color(tr("Cops", "Копы"), radar.cop_color);
            edited |= color(tr("Robbers", "Преступники"), radar.robber_color);
            edited |= color(tr("Other players", "Другие игроки"), radar.neutral_color);
            edited |= color(tr("Loot", "Лут"), radar.loot_color);
        });
    });
    return edited;
}

bool MenuModule::loot_tab() {
    auto& loot = settings_.loot;
    const auto language = settings_.language;
    const auto language_index = static_cast<std::size_t>(language);
    const auto& rows = loot_catalog();
    const std::array categories{tr("Weapon", "Оружие"), tr("Item", "Предмет"), tr("Cargo", "Груз"),
                                tr("Container", "Контейнер"), tr("Loot point", "Точка лута")};
    float category_width = 0.0f;
    for (const char* label : categories) {
        category_width = (std::max)(category_width, ImGui::CalcTextSize(label).x);
    }
    const auto accent = ImGui::ColorConvertU32ToFloat4(settings_.accent_color);
    const auto opacity = settings_.menu_opacity;
    const bool columns = ImGui::GetContentRegionAvail().x >= 490.0f;
    const auto height = ImGui::GetContentRegionAvail().y;
    bool edited = false;

    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4.0f, 3.0f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.078f, 0.078f, 0.078f, opacity});
    ImGui::PushStyleColor(ImGuiCol_Text, {1.0f, 1.0f, 1.0f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, {0.533f, 0.533f, 0.533f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_CheckMark, accent);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, {0.039f, 0.039f, 0.039f, opacity});
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, {0.20f, 0.20f, 0.20f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_Tab, {0.10f, 0.10f, 0.10f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_TabSelected, {accent.x * 0.30f, accent.y * 0.30f, accent.z * 0.30f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, accent);

    const bool table = columns && ImGui::BeginTable("LootPanels", 2, ImGuiTableFlags_SizingStretchProp |
                                                   ImGuiTableFlags_NoSavedSettings);
    if (table) {
        ImGui::TableSetupColumn("Global", ImGuiTableColumnFlags_WidthStretch, 0.34f);
        ImGui::TableSetupColumn("Catalog", ImGuiTableColumnFlags_WidthStretch, 0.66f);
        ImGui::TableNextColumn();
    }
    if (ImGui::BeginChild("LootGlobal", {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
        section_heading(tr("GLOBAL SETTINGS", "ОБЩИЕ НАСТРОЙКИ"), settings_.accent_color);
        edited |= setting_toggle(tr("Enable", "Включить"), loot.enabled);
        edited |= slider(tr("Max. distance", "Макс. дистанция"), loot.max_distance, 1.0f, 10000.0f, "%.0f m",
                         ImGuiSliderFlags_Logarithmic);
        edited |= setting_toggle(tr("Show distance", "Показывать дистанцию"), loot.distance);
        edited |= setting_toggle(tr("Hide empty", "Скрыть пустые"), loot.hide_empty);
        edited |= setting_toggle(tr("Dropped loot", "Выпавший лут"), loot.bags);
        edited |= setting_toggle(tr("Show price", "Показывать цену"), loot.show_price);
        edited |= slider(tr("Min. price", "Мин. цена"), loot.min_price, 0.0f, 2'000'000.0f, "$%.0f",
                         ImGuiSliderFlags_Logarithmic);
        if (ImGui::CollapsingHeader(tr("Objects###LootObjects", "Объекты###LootObjects"))) {
            edited |= setting_toggle(tr("Containers", "Контейнеры"), loot.containers);
            edited |= setting_toggle(tr("Cargo", "Грузы"), loot.cargo);
            edited |= setting_toggle(tr("Weapons", "Оружие"), loot.weapons);
            edited |= setting_toggle(tr("Ground items", "Предметы на земле"), loot.ground_items);
            edited |= setting_toggle(tr("Vehicle trunks", "Багажники"), loot.vehicles);
            edited |= setting_toggle(tr("Show contents", "Содержимое"), loot.show_contents);
        }
        if (ImGui::CollapsingHeader(tr("Appearance###LootAppearance", "Оформление###LootAppearance"))) {
            edited |= setting_toggle(tr("Markers", "Маркеры"), loot.markers);
            if (loot.markers) edited |= slider(tr("Marker size", "Размер маркера"), loot.marker_size, 1.0f, 15.0f, "%.1f px");
            edited |= setting_toggle(tr("Item names", "Названия"), loot.names);
            edited |= color(tr("Bags", "Сумки"), loot.bag_color);
            edited |= color(tr("Containers", "Контейнеры"), loot.container_color);
            edited |= color(tr("Cargo", "Грузы"), loot.cargo_color);
            edited |= color(tr("Ground items", "Предметы на земле"), loot.ground_color);
            edited |= color(tr("Vehicles", "Транспорт"), loot.vehicle_color);
        }
    }
    ImGui::EndChild();

    if (table) ImGui::TableNextColumn();
    const float right_height = (std::max)(height - 9.0f, 390.0f);
    if (ImGui::BeginChild("LootCatalog", {0, right_height}, ImGuiChildFlags_Borders)) {
        section_heading(tr("ITEM FILTERS", "ФИЛЬТРЫ ПРЕДМЕТОВ"), settings_.accent_color);
        constexpr std::array rarity_colors{0xFF888888u, 0xFF67D56Bu, 0xFFFFA64Du,
                                          0xFFD08CDBu, 0xFF32B6FFu, 0xFF3535FFu};
        std::array<int, 6> total{}, enabled{};
        for (const auto& row : rows) {
            if (row.rarity < 1) continue;
            const auto i = static_cast<std::size_t>(row.rarity - 1);
            ++total[i];
            if (loot.item_enabled(row.item)) ++enabled[i];
        }
        const std::array rarity_labels{tr("Common", "Обычный"), tr("Uncommon", "Необычный"),
                                      tr("Rare", "Редкий"), tr("Epic", "Эпик"),
                                      tr("Legendary", "Легенд."), tr("Exotic", "Экзот.")};
        const float edge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        for (std::size_t i = 0; i < rarity_labels.size(); ++i) {
            const char* label = rarity_labels[i];
            const float button_width = ImGui::CalcTextSize(label).x + 2 * ImGui::GetStyle().FramePadding.x;
            if (i != 0 && ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetStyle().ItemSpacing.x
                         + button_width <= edge) ImGui::SameLine();
            const auto tint = ImGui::ColorConvertU32ToFloat4(rarity_colors[i]);
            const bool all = total[i] != 0 && enabled[i] == total[i];
            const float fill = all ? 0.27f : enabled[i] ? 0.14f : 0.05f;
            ImGui::PushStyleColor(ImGuiCol_Text, tint);
            ImGui::PushStyleColor(ImGuiCol_Button, {0.04f + tint.x * fill, 0.04f + tint.y * fill, 0.04f + tint.z * fill, 1.0f});
            ImGui::BeginDisabled(total[i] == 0);
            if (ImGui::Button(label)) {
                for (const auto& row : rows) {
                    if (row.rarity != static_cast<int>(i + 1)) continue;
                    if (all) loot.disabled_items.insert(row.item.normalized_id);
                    else loot.disabled_items.erase(row.item.normalized_id);
                }
                edited = true;
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor(2);
            ImGui::SetItemTooltip("%d / %d", enabled[i], total[i]);
        }
        ImGui::BeginDisabled(loot.disabled_items.empty());
        if (ImGui::SmallButton(tr("Enable all", "Включить все"))) {
            loot.disabled_items.clear();
            edited = true;
        }
        ImGui::EndDisabled();
        if (ImGui::BeginTabBar("LootGroups", ImGuiTabBarFlags_FittingPolicyScroll)) {
            const std::array tabs{
                tr("Weapons###LootWeapons", "Оружие###LootWeapons"),
                tr("Items###LootItems", "Предметы###LootItems"),
                tr("Cargo / Containers###LootContainers", "Грузы / Контейнеры###LootContainers"),
                tr("All###LootAll", "Все###LootAll")};
            for (std::size_t tab = 0; tab < tabs.size(); ++tab) {
                if (!ImGui::BeginTabItem(tabs[tab])) continue;
                auto& search = loot_search_[tab];
                ImGui::SetNextItemWidth(-1.0f);
                const bool search_changed = ImGui::InputTextWithHint("##search", tr("Search name or ID", "Поиск по названию или ID"),
                                                                      search.data(), search.size());
                const auto query = folded_text(search.data());
                const auto group = static_cast<LootGroup>(tab);
                std::vector<const LootCatalogRow*> matches;
                matches.reserve(rows.size());
                for (const auto& row : rows) {
                    if (group != LootGroup::all && group != row.group) continue;
                    if (query.empty() || row.folded_id.find(query) != std::wstring::npos ||
                        row.folded_names[0].find(query) != std::wstring::npos ||
                        row.folded_names[1].find(query) != std::wstring::npos) matches.push_back(&row);
                }
                if (search_changed) ImGui::SetNextWindowScroll({0.0f, 0.0f});
                if (ImGui::BeginChild("LootRows", {0, 0}, ImGuiChildFlags_Borders)) {
                    const bool separate_columns = ImGui::GetContentRegionAvail().x >= 350.0f;
                    if (matches.empty()) {
                        ImGui::TextDisabled("%s", tr("No matches", "Ничего не найдено"));
                    } else if (ImGui::BeginTable("LootRowsTable", separate_columns ? 4 : 2, ImGuiTableFlags_SizingFixedFit |
                                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_NoSavedSettings, {0.0f, 0.0f})) {
                        ImGui::TableSetupColumn("Toggle", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                        if (separate_columns) {
                            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, category_width);
                        }
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                        if (separate_columns) {
                            ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("$123456–123456").x);
                        }
                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(matches.size()));
                        while (clipper.Step()) {
                            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                                const auto& row = *matches[static_cast<std::size_t>(i)];
                                const auto& id = row.item.normalized_id;
                                ImGui::PushID(id.c_str());
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                bool visible = loot.item_enabled(row.item);
                                if (ImGui::Checkbox("##enabled", &visible)) {
                                    if (visible) loot.disabled_items.erase(id);
                                    else loot.disabled_items.insert(id);
                                    edited = true;
                                }
                                const char* category = categories[static_cast<std::size_t>(row.category)];
                                if (separate_columns) {
                                    ImGui::TableNextColumn();
                                    ImGui::AlignTextToFramePadding();
                                    ImGui::TextDisabled("%s", category);
                                }
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                const auto square = ImGui::GetCursorScreenPos();
                                const auto square_color = row.rarity > 0
                                    ? rarity_colors[static_cast<std::size_t>(row.rarity - 1)] : 0xFF555555u;
                                const float square_y = square.y + (ImGui::GetTextLineHeight() - 10.0f) * 0.5f;
                                ImGui::GetWindowDrawList()->AddRectFilled({square.x, square_y},
                                    {square.x + 10.0f, square_y + 10.0f}, square_color, 2.0f);
                                ImGui::Dummy({10.0f, ImGui::GetTextLineHeight()});
                                ImGui::SameLine(0.0f, 6.0f);
                                if (!visible) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                                ImGui::TextUnformatted(row.names[language_index].c_str());
                                if (!visible) ImGui::PopStyleColor();
                                ImGui::SetItemTooltip("%s\n%s · %s", row.names[language_index].c_str(), id.c_str(),
                                                      game_data::rarity_name(row.rarity, language));
                                const auto& price = row.prices[language_index];
                                if (separate_columns) {
                                    ImGui::TableNextColumn();
                                    ImGui::AlignTextToFramePadding();
                                    ImGui::TextDisabled("%s", price.c_str());
                                } else if (price.empty()) {
                                    ImGui::TextDisabled("%s", category);
                                } else {
                                    ImGui::TextDisabled("%s · %s", category, price.c_str());
                                }
                                ImGui::PopID();
                            }
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
    if (table) ImGui::EndTable();
    ImGui::PopStyleColor(9);
    ImGui::PopStyleVar(2);
    if (edited) layout_save_pending_ = true;
    return edited;
}

bool MenuModule::indicators_tab() {
    auto& options = settings_.indicators;
    bool edited = ImGui::Checkbox(tr("Direction arrows", "Стрелки направления"), &options.enabled);
    edited |= slider(tr("Arrow radius", "Радиус стрелок"), options.arrow_radius, 32.0f, 1000.0f, "%.0f px");
    edited |= slider(tr("Arrow size", "Размер стрелок"), options.arrow_size, 4.0f, 24.0f, "%.0f px");
    edited |= slider(tr("Maximum distance", "Макс. дистанция"), options.max_distance, 1.0f, 10000.0f, "%.0f m",
                     ImGuiSliderFlags_Logarithmic);
    edited |= ImGui::Checkbox(tr("Only off-screen players", "Только игроки вне экрана"), &options.show_only_offscreen);
    edited |= ImGui::Checkbox(tr("Show weapon icon on arrows", "Иконка оружия на стрелках"), &options.arrow_show_weapon_icon);
    ImGui::TextWrapped(tr("Arrows are hidden while this menu is open.", "Стрелки скрыты, пока меню открыто."));
    return edited;
}

bool MenuModule::settings_tab(const Stats& stats) {
    const auto accent = settings_.accent_color;
    bool edited = false;
    settings_columns(490.0f, [&] {
        settings_card("OverlayAppearance", tr("APPEARANCE", "ОФОРМЛЕНИЕ"), accent, [&] {
            edited |= slider(tr("Menu opacity", "Прозрачность меню"), settings_.menu_opacity, 0.30f, 1.0f, "%.2f");
            edited |= color(tr("Accent color", "Цвет акцента"), settings_.accent_color, false);
            edited |= slider(tr("Weapon icon size", "Размер иконки оружия"),settings_.weapon_icon_size,16.0f,64.0f,"%.0f px");
            edited |= setting_toggle(tr("Crosshair", "Прицел"), settings_.crosshair);
        });
        settings_card("OverlayShortcuts", tr("SHORTCUTS", "ГОРЯЧИЕ КЛАВИШИ"), accent, [&] {
            key_button(tr("Toggle menu", "Открыть / закрыть меню"), Binding::menu, settings_.menu_key);
            key_button(tr("Exit overlay", "Выгрузить оверлей"), Binding::exit, settings_.exit_key);
        });
    }, [&] {
        if constexpr (scarlex::build::diagnostics) settings_card("OverlayDiagnostics", tr("DIAGNOSTICS", "ДИАГНОСТИКА"), accent, [&] {
            edited |= setting_toggle(tr("Log reader and mesh diagnostics", "Логировать чтение и модели"), settings_.diagnostics);
            edited |= setting_toggle(tr("Log snapshot performance", "Логировать время чтения"), settings_.performance_log);
            if (stats.snapshot_ready) {
                ImGui::Text(tr("%zu players  /  %zu loot items", "Игроки: %zu  /  Лут: %zu"), stats.entities, stats.loot);
                ImGui::Text(tr("Read %.2f ms  /  Age %.2f ms  /  %llu IOCTLs", "Чтение %.2f мс  /  Возраст %.2f мс  /  IOCTL %llu"), stats.read_ms,
                            stats.snapshot_age_ms, static_cast<unsigned long long>(stats.ioctls));
            } else {
                ImGui::TextDisabled(tr("Waiting for a snapshot", "Ожидание снимка"));
            }
        });
        settings_card("OverlayProfile", tr("PROFILE", "ПРОФИЛЬ"), accent, [&] {
            edited |= setting_toggle(tr("Save when closing the menu or exiting", "Сохранять при закрытии или выходе"), settings_.autosave);
            if (ImGui::Button(tr("Save", "Сохранить"))) {
                save_settings();
                edited = false;
            }
            if (ImGui::Button(tr("Load", "Загрузить")) && load_settings()) edited = false;
            if (ImGui::Button(tr("Reset defaults", "Сбросить настройки"))) {
                settings_ = {};
                synchronize_hotkeys();
                status_ = tr("Defaults restored.", "Настройки сброшены.");
                edited = true;
            }
            if constexpr (scarlex::build::diagnostics) {
                const auto utf8 = path_.u8string();
                ImGui::TextWrapped("%s", reinterpret_cast<const char*>(utf8.c_str()));
            }
            ImGui::Spacing();
            if (ImGui::Button(tr("Exit overlay", "Выгрузить оверлей"))) ImGui::OpenPopup(tr("Exit overlay?", "Выгрузить оверлей?"));
            if (ImGui::BeginPopupModal(tr("Exit overlay?", "Выгрузить оверлей?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(tr("Close the overlay and stop its reader?", "Закрыть оверлей и остановить чтение?"));
                if (ImGui::Button(tr("Exit", "Выгрузить"), {95.0f, 0.0f})) {
                    exiting_ = true;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::Button(tr("Cancel", "Отмена"), {95.0f, 0.0f})) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        });
    });
    return edited;
}

void MenuModule::radar_handle(RECT work, RECT drawable) {
    const auto area = RadarModule(settings_.radar).placement(work, drawable);
    if (!area) return;
    const float width = (std::min)(110.0f, area->radius * 2.0f);
    constexpr float height = 25.0f;
    const float left = std::clamp(area->x - width * 0.5f, area->min_x,
        (std::max)(area->min_x, area->max_x + 1.0f - width));
    const float top = std::clamp(area->y - area->radius - 9.0f, area->min_y,
        (std::max)(area->min_y, area->max_y + 1.0f - height));
    ImGui::SetNextWindowPos({left, top});
    ImGui::SetNextWindowSize({width, height});
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    if (ImGui::Begin("##RadarDragHandle", nullptr, flags)) {
        const char* label = width >= 82.0f ? tr("Drag radar", "Двигать") : "+";
        ImGui::Button(label, {width, height});
        const auto mouse = ImGui::GetIO().MousePos;
        if (ImGui::IsItemActivated()) {
            radar_grab_x_ = mouse.x;
            radar_grab_y_ = mouse.y;
            radar_start_x_ = area->x;
            radar_start_y_ = area->y;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            auto& radar = settings_.radar;
            radar.position_x = dragged_fraction(mouse.x, radar_grab_x_, radar_start_x_, area->min_x, area->max_x);
            radar.position_y = dragged_fraction(mouse.y, radar_grab_y_, radar_start_y_, area->min_y, area->max_y);
            radar.custom_position = radar.position_in_client = dirty_ = true;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void MenuModule::on_render(RECT visible, RECT drawable, const Stats& stats, std::uint64_t logo, ImFont* font,
                          TextureManager* textures) {
    if (!open_) return;
    const float width = static_cast<float>(visible.right - visible.left);
    const float height = static_cast<float>(visible.bottom - visible.top);
    if (width < 100.0f || height < 100.0f) return;
    const float x = static_cast<float>(visible.left);
    const float y = static_cast<float>(visible.top);
    const ImVec2 maximum{width - 16.0f, height - 16.0f};
    auto accent = ImGui::ColorConvertU32ToFloat4(settings_.accent_color);
    accent.w = 1.0f;
    const float opacity = settings_.menu_opacity;
    const auto tint = [&](float intensity, float alpha = 1.0f) {
        return ImVec4{accent.x * intensity, accent.y * intensity, accent.z * intensity, alpha};
    };
    if (font) ImGui::PushFont(font);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 11.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16.0f, 14.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12.0f, 7.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10.0f, 6.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.039f, 0.039f, 0.039f, opacity});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.078f, 0.078f, 0.083f, opacity});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4{0.08f, 0.08f, 0.09f, opacity});
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{0.26f, 0.26f, 0.28f, 0.57f});
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.96f, 0.95f, 0.96f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4{0.63f, 0.62f, 0.67f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{0.13f, 0.13f, 0.15f, opacity});
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, tint(0.23f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, tint(0.36f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.16f, 0.16f, 0.18f, opacity});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tint(0.34f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, tint(0.51f));
    ImGui::PushStyleColor(ImGuiCol_Header, tint(0.30f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, tint(0.51f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, tint(0.67f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, tint(0.80f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, accent);
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4{0.24f, 0.24f, 0.27f, 0.61f});
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4{0.05f, 0.05f, 0.06f, 0.45f});
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4{0.25f, 0.25f, 0.29f, 0.90f});
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, tint(0.50f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, accent);

    radar_handle(visible, drawable);
    ImGui::SetNextWindowPos({x + width * 0.5f, y + height * 0.5f},
                            ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({(std::min)(970.0f, maximum.x), (std::min)(730.0f, maximum.y)},
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({(std::min)(360.0f, maximum.x), (std::min)(230.0f, maximum.y)}, maximum);
    bool stay_open = true;
    if (ImGui::Begin("SCARLEX###Overlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar)) {
        const auto open_size = ImGui::GetWindowSize();
        const auto open_position = ImGui::GetWindowPos();
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 clamped{std::clamp(open_position.x, x, (std::max)(x, x + width - open_size.x)),
                                 std::clamp(open_position.y, y, (std::max)(y, y + height - open_size.y))};
            if (open_position.x != clamped.x || open_position.y != clamped.y) ImGui::SetWindowPos(clamped);
        }
        const std::array pages{tr("Aimbot", "Аим"), tr("ESP", "Визуал"), tr("Radar", "Радар"),
                               tr("Loot", "Лут"), tr("Indicators", "Индикаторы"), tr("Settings", "Настройки")};
        const float sidebar = std::clamp(open_size.x * 0.20f, 125.0f, 172.0f);
        const auto card = ImGuiChildFlags_Borders;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.062f, 0.056f, 0.064f, opacity});
        if (ImGui::BeginChild("Navigation", {sidebar, 0.0f}, card)) {
            const float brand_width = (std::max)(50.0f, ImGui::GetContentRegionAvail().x);
            if (logo != 0) {
                ImGui::Image(logo, {brand_width, brand_width * 0.24f});
            } else {
                ImGui::TextColored(accent, "SCARLEX");
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            for (int i = 0; i < static_cast<int>(std::size(pages)); ++i) {
                const bool selected = i == static_cast<int>(page_);
                ImGui::PushID(i);
                if (navigation_button(pages[i], i, selected, accent)) page_ = static_cast<Page>(i);
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const auto content = ImGui::GetContentRegionAvail();
        const float footer = ImGui::GetTextLineHeightWithSpacing() * (status_.empty() ? 1.0f : 2.0f);
        ImGui::BeginGroup();
        const auto header = ImGui::GetCursorScreenPos();
        const float title_width = (std::max)(50.0f, content.x - 89.0f);
        ImGui::InvisibleButton("##PanelDrag", {title_width, 42.0f});
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(header, {header.x + title_width, header.y + 42.0f}, true);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.65f,
                      {header.x + 2.0f, header.y + 7.0f}, ImGui::GetColorU32(ImGuiCol_Text),
                      pages[static_cast<int>(page_)]);
        draw->PopClipRect();
        const bool dragging = ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f);
        ImGui::SameLine();
        ImGui::SetCursorScreenPos({header.x + content.x - 77.0f, header.y + 7.0f});
        if (language_button(settings_.language)) {
            settings_.language = settings_.language == game_data::Language::russian
                ? game_data::Language::english : game_data::Language::russian;
            dirty_ = layout_save_pending_ = true;
        }
        ImGui::SameLine();
        ImGui::SetCursorScreenPos({header.x + content.x - 31.0f, header.y + 3.0f});
        stay_open = !close_menu_button(tr("Close menu", "Закрыть меню"));
        ImGui::Separator();
        const float body_height = ImGui::GetContentRegionAvail().y - footer;
        ImGui::PushID(static_cast<int>(page_));
        if (ImGui::BeginChild("Controls", {content.x, (std::max)(46.0f, body_height)},
                              ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground)) {
            switch (page_) {
            case Page::aim: dirty_ |= aim_tab(); break;
            case Page::esp: dirty_ |= esp_tab(textures); break;
            case Page::radar: dirty_ |= radar_tab(visible, drawable); break;
            case Page::loot: dirty_ |= loot_tab(); break;
            case Page::indicators: dirty_ |= indicators_tab(); break;
            case Page::settings: dirty_ |= settings_tab(stats); break;
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
        if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
        ImGui::TextDisabled(tr("%s  Menu     %s  Exit", "%s  Меню     %s  Выход"),
                            key_name(settings_.menu_key).c_str(), key_name(settings_.exit_key).c_str());
        ImGui::EndGroup();
        if (dragging) {
            const auto delta = ImGui::GetIO().MouseDelta;
            ImGui::SetWindowPos({
                std::clamp(open_position.x + delta.x, x, (std::max)(x, x + width - open_size.x)),
                std::clamp(open_position.y + delta.y, y, (std::max)(y, y + height - open_size.y))});
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(23);
    ImGui::PopStyleVar(9);
    if (font) ImGui::PopFont();
    if (layout_save_pending_ && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        layout_save_pending_=false;
        static_cast<void>(save_settings());
    }
    if (!stay_open) close_panel();
}

void MenuModule::close_panel() {
    layout_drag_item_=-1;layout_dragging_=false;
    open_ = false;
    binding_ = Binding::none;
    finish();
}

void MenuModule::finish() {
    const bool save=layout_save_pending_ || (dirty_ && settings_.autosave);
    layout_save_pending_=false;
    if (save && !save_settings()) {
        scarlex::diagnostic_fprintf(stderr, "[MENU] %s\n", status_.c_str());
    }
}

bool MenuModule::load_settings() {
    try {
        std::ifstream file(path_, std::ios::binary);
        if (!file) throw std::runtime_error("Could not open settings file.");
        const auto document = nlohmann::json::parse(file);
        if (!document.is_object() || document.value("version", 0) != 1) {
            throw std::runtime_error("Unsupported settings format.");
        }
        auto loaded = document.get<MenuSettings>();
        if (const auto layout=document.find("esp_layout");layout!=document.end())
            loaded.esp.esp_layout=layout->get<game_entities::EspLayout>();
        if (const auto loot = document.find("loot"); loot != document.end() && !loot->contains("weapons")) {
            // Keep custom colors while adopting the new palette for old defaults.
            if (loaded.loot.bag_color == 0xFFFFFFFFu) loaded.loot.bag_color = LootSettings{}.bag_color;
            if (loaded.loot.cargo_color == 0xFF42C8FFu) loaded.loot.cargo_color = LootSettings{}.cargo_color;
        }
        validate(loaded);
        settings_ = loaded;
        binding_ = Binding::none;
        synchronize_hotkeys();
        status_ = tr("Settings loaded.", "Настройки загружены.");
        dirty_ = false;
        layout_save_pending_=false;
        layout_drag_item_=-1;layout_dragging_=false;
        return true;
    } catch (const std::exception& error) {
        status_ = std::string(tr("Load failed: ", "Ошибка загрузки: ")) + error.what();
        return false;
    }
}

bool MenuModule::save_settings() {
    auto temporary = path_;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    try {
        auto serialized = settings_;
        validate(serialized);
        nlohmann::json document = serialized;
        document["version"] = 1;
        document["esp_layout"] = serialized.esp.esp_layout;
        if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path());
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file << document.dump(2) << '\n';
        file.close();
        if (!MoveFileExW(temporary.c_str(), path_.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Replace settings file");
        }
        status_ = tr("Settings saved.", "Настройки сохранены.");
        dirty_ = false;
        layout_save_pending_=false;
        return true;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        status_ = std::string(tr("Save failed: ", "Ошибка сохранения: ")) + error.what();
        return false;
    }
}

