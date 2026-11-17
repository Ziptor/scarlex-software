#pragma once

#include "game_entities.h"
#include "core/types.h"
#include <array>
#include <limits>

namespace game_entities {

enum class EspItem : std::size_t { role, weapon, distance, name, weapon_icon, count };
inline constexpr std::size_t esp_item_count=static_cast<std::size_t>(EspItem::count);
inline auto esp_elements(EspLayout& l) { return std::array{&l.role,&l.weapon,&l.distance,&l.name,&l.weapon_icon}; }
inline auto esp_elements(const EspLayout& l) { return std::array{&l.role,&l.weapon,&l.distance,&l.name,&l.weapon_icon}; }
struct EspItemSize { float width{},height{};bool visible{}; };
struct EspItemRect {
    float x{},y{},width{},height{};bool visible{};
    core::Vec2 center() const noexcept { return {x+width*0.5f,y+height*0.5f}; }
};
using EspItemSizes=std::array<EspItemSize,esp_item_count>;
using EspItemRects=std::array<EspItemRect,esp_item_count>;

inline EspAnchor valid_esp_anchor(EspAnchor anchor,bool icon) noexcept {
    if (anchor>EspAnchor::top_right) return icon ? EspAnchor::top_right : EspAnchor::bottom_center;
    return !icon && anchor>=EspAnchor::top_left ? EspAnchor::top_center : anchor;
}
inline const char* esp_anchor_key(EspAnchor anchor) noexcept {
    switch(anchor) {
    case EspAnchor::top_center:return "top_center";
    case EspAnchor::bottom_center:return "bottom_center";
    case EspAnchor::top_left:return "top_left";
    case EspAnchor::top_right:return "top_right";
    }
    return "bottom_center";
}

// Both preview and live ESP use the same slots and automatic row stacking.
inline EspItemRects arrange_esp_layout(const EspLayout& layout,const EspItemSizes& sizes,
                                      float left,float top,float right,float bottom) noexcept {
    EspItemRects result{};
    const auto elements=esp_elements(layout);
    constexpr float gap=5.0f,spacing=4.0f;
    float above=top-gap,below=bottom+gap;
    constexpr std::array top_order{1u,4u,0u,2u,3u};
    constexpr std::array bottom_order{0u,4u,1u,2u,3u};
    const auto place=[&](std::size_t i,EspAnchor anchor,float& cursor) {
        if (!sizes[i].visible || !elements[i]->visible ||
            valid_esp_anchor(elements[i]->anchor,i==4)!=anchor) return;
        const auto& size=sizes[i];
        const float y=anchor==EspAnchor::top_center ? cursor-size.height : cursor;
        result[i]={(left+right-size.width)*0.5f,y,size.width,size.height,true};
        cursor+=anchor==EspAnchor::top_center ? -(size.height+spacing) : size.height+spacing;
    };
    for (auto i:top_order) place(i,EspAnchor::top_center,above);
    for (auto i:bottom_order) place(i,EspAnchor::bottom_center,below);
    constexpr std::size_t icon=static_cast<std::size_t>(EspItem::weapon_icon);
    const auto anchor=valid_esp_anchor(elements[icon]->anchor,true);
    if (sizes[icon].visible && elements[icon]->visible && anchor>=EspAnchor::top_left) {
        const auto& size=sizes[icon];
        result[icon]={anchor==EspAnchor::top_left ? left-gap-size.width : right+gap,
                      top,size.width,size.height,true};
    }
    return result;
}

inline EspAnchor nearest_esp_anchor(EspLayout layout,std::size_t item,const EspItemSizes& sizes,
                                    float left,float top,float right,float bottom,core::Vec2 drop) noexcept {
    if(item>=esp_item_count) return EspAnchor::bottom_center;
    EspAnchor best=EspAnchor::bottom_center;
    float distance=(std::numeric_limits<float>::max)();
    const int count=item==static_cast<std::size_t>(EspItem::weapon_icon) ? 4 : 2;
    for (int zone=0;zone<count;++zone) {
        const auto anchor=static_cast<EspAnchor>(zone);
        esp_elements(layout)[item]->anchor=anchor;
        const auto rect=arrange_esp_layout(layout,sizes,left,top,right,bottom)[item];
        const auto center=rect.center();
        const float dx=drop.x-center.x,dy=drop.y-center.y,d=dx*dx+dy*dy;
        if (d<distance) {distance=d;best=anchor;}
    }
    return best;
}

} // namespace game_entities
