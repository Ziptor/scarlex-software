bool MenuModule::layout_editor(TextureManager* textures) {
    using namespace game_entities;
    auto& esp=settings_.esp;auto& layout=esp.esp_layout;
    const auto elements=esp_elements(layout);
    const std::array names{tr("Role","Роль"),tr("Weapon text","Название оружия"),tr("Distance","Дистанция"),
                           tr("Name","Ник"),tr("Weapon icon","Иконка оружия")};
    constexpr std::array ids{"##EspLayoutRole","##EspLayoutWeapon","##EspLayoutDistance","##EspLayoutName","##EspLayoutWeaponIcon"};
    const bool show_role = esp.role_labels && layout.role.visible;
    const char* role_preview = esp.team_labels ? (show_role ? "cop  [Clan] #0" : "[Clan] #0") : "cop";
    const std::array samples{role_preview,"DRM-14 Mk1 [30]","42 m",tr("Player","Игрок")};
    const std::array zone_names{tr("Top center","Сверху по центру"),tr("Bottom center","Снизу по центру"),
                                tr("Top left","Слева сверху"),tr("Top right","Справа сверху")};
    bool edited{};
    ImGui::TextWrapped("%s",tr("Drag to dock labels above or below. The weapon icon also docks to the upper corners.",
        "Перетащите подпись вверх или вниз. Иконку оружия можно поставить и в верхние углы."));
    edited |= ImGui::Combo(tr("Weapon display","Отображение оружия"),&esp.esp_weapon_display_mode,
                           tr("Text\0Icon\0Both\0","Текст\0Иконка\0Оба\0"));
    if(ImGui::BeginChild("EspLayoutCanvas",{0,420},ImGuiChildFlags_Borders,
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const auto origin=ImGui::GetCursorScreenPos(),space=ImGui::GetContentRegionAvail();
        const float box_width=std::clamp(space.x*0.45f, 40.0f, 128.0f);
        const float box_height=std::clamp(space.y-145.0f, 135.0f, 256.0f);
        const float scale=box_height/272.0f;
        const float cx=origin.x+space.x*0.5f,top=origin.y+(space.y-box_height)*0.5f;
        const float left=cx-box_width*0.5f,right=cx+box_width*0.5f,bottom=top+box_height;
        auto* draw=ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin,{origin.x+space.x,origin.y+space.y},IM_COL32(15,17,21,245),5);
        draw->AddLine({cx,origin.y+8},{cx,origin.y+space.y-8},IM_COL32(90,100,115,22));
        draw_character_preview(draw,{cx-86*scale,top-10*scale},scale);
        draw->AddRect({left,top},{right,bottom},IM_COL32(120,130,145,115),2,0,1);
        const int mode=std::clamp(esp.esp_weapon_display_mode,0,2);
        const auto icon=mode!=0 && textures ? textures->WeaponIcon("drm14_m1") : ImTextureID{};
        const auto image_size=icon ? textures->IconSize(icon,settings_.weapon_icon_size) : ImVec2{};
        EspItemSizes sizes{};
        const float line=ImGui::GetTextLineHeight();
        for(std::size_t i=0;i<4;++i) sizes[i]={ImGui::CalcTextSize(samples[i]).x,line,elements[i]->visible};
        sizes[0].visible = show_role || esp.team_labels;
        sizes[1].visible &= esp.weapon_labels && (mode!=1 || !icon);
        sizes[1].height=line*2+2;
        sizes[2].visible &= esp.distance_labels;
        sizes[4]={image_size.x,image_size.y,esp.weapon_labels && icon!=ImTextureID{} && layout.weapon_icon.visible};
        auto preview_layout = layout;
        preview_layout.role.visible = sizes[0].visible;
        auto rects=arrange_esp_layout(preview_layout,sizes,left,top,right,bottom);
        layout_selected_=std::clamp(layout_selected_,0,4);
        const int first=layout_selected_;
        for(int pass=0;pass<5;++pass) {
            const int index=pass==0 ? first : pass<=first ? pass-1 : pass;
            const auto i=static_cast<std::size_t>(index);
            if(!rects[i].visible) continue;
            const auto& r=rects[i];
            ImGui::SetCursorScreenPos({r.x-2,r.y-2});
            ImGui::InvisibleButton(ids[i],{r.width+4,r.height+4},ImGuiButtonFlags_MouseButtonLeft);
            if(ImGui::IsItemActivated()) {
                layout_selected_=layout_drag_item_=index;layout_dragging_=false;
                const auto center=r.center();const auto mouse=ImGui::GetIO().MousePos;
                layout_drag_x_=center.x;layout_drag_y_=center.y;
                layout_grab_x_=mouse.x-center.x;layout_grab_y_=mouse.y-center.y;
                layout_drop_anchor_=elements[i]->anchor;
            }
            if(ImGui::IsItemActive() && ImGui::IsMouseDragging(0,3.0f)) {
                const auto mouse=ImGui::GetIO().MousePos;
                const auto clamp_drag=[](float mouse_axis,float origin_axis,float span,float item_size) {
                    const float inset=(std::min)(item_size*0.5f+4.0f,span*0.5f);
                    return std::clamp(mouse_axis,origin_axis+inset,origin_axis+span-inset);
                };
                layout_drag_x_=clamp_drag(mouse.x-layout_grab_x_,origin.x,space.x,r.width);
                layout_drag_y_=clamp_drag(mouse.y-layout_grab_y_,origin.y,space.y,r.height);
                layout_dragging_=true;
                layout_drop_anchor_=nearest_esp_anchor(preview_layout,i,sizes,left,top,right,bottom,
                                                        {layout_drag_x_,layout_drag_y_});
            }
            if(ImGui::IsItemActive() || ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            if(ImGui::IsItemHovered() && !layout_dragging_)
                ImGui::SetTooltip("%s - %s",names[i],zone_names[static_cast<std::size_t>(elements[i]->anchor)]);
        }
        if(layout_drag_item_>=0 && !ImGui::IsMouseDown(0)) {
            const auto i=static_cast<std::size_t>(layout_drag_item_);
            if(layout_dragging_ && elements[i]->anchor!=layout_drop_anchor_) {
                elements[i]->anchor=layout_drop_anchor_;edited=true;
            }
            layout_drag_item_=-1;layout_dragging_=false;
            preview_layout = layout;
            preview_layout.role.visible = sizes[0].visible;
            rects=arrange_esp_layout(preview_layout,sizes,left,top,right,bottom);
        }
        if(layout_dragging_ && layout_drag_item_>=0) {
            const auto item=static_cast<std::size_t>(layout_drag_item_);
            const int count=item==4 ? 4 : 2;
            for(int zone=0;zone<count;++zone) {
                auto candidate=preview_layout;esp_elements(candidate)[item]->anchor=static_cast<EspAnchor>(zone);
                const auto r=arrange_esp_layout(candidate,sizes,left,top,right,bottom)[item];
                const bool selected=static_cast<EspAnchor>(zone)==layout_drop_anchor_;
                const auto center=r.center();
                const float width=(std::max)(r.width+12,82.0f),height=(std::max)(r.height+10,28.0f);
                const ImVec2 a{center.x-width*0.5f,center.y-height*0.5f},b{center.x+width*0.5f,center.y+height*0.5f};
                draw->AddRectFilled(a,b,selected ? IM_COL32(155,35,42,120) : IM_COL32(50,60,72,90),4);
                draw->AddRect(a,b,selected ? settings_.accent_color : IM_COL32(110,120,140,100),4,0,selected ? 2.0f : 1.0f);
                draw->AddText({a.x,a.y-15},selected ? settings_.accent_color : IM_COL32(155,160,175,160),zone_names[zone]);
            }
        }
        for(std::size_t i=0;i<rects.size();++i) {
            if(!rects[i].visible) continue;
            auto r=rects[i];
            const bool dragging=layout_dragging_ && static_cast<int>(i)==layout_drag_item_;
            if(dragging) {r.x=layout_drag_x_-r.width*0.5f;r.y=layout_drag_y_-r.height*0.5f;}
            const bool selected=static_cast<int>(i)==layout_selected_;
            draw->AddRectFilled({r.x-2,r.y-2},{r.x+r.width+2,r.y+r.height+2},IM_COL32(10,11,15,180),3);
            draw->AddRect({r.x-2,r.y-2},{r.x+r.width+2,r.y+r.height+2},selected ? settings_.accent_color : IM_COL32(100,110,125,60),3);
            if(i==4) draw->AddImage(icon,{r.x,r.y},{r.x+r.width,r.y+r.height});
            else {
                const auto size=ImGui::CalcTextSize(samples[i]);
                draw->AddText({r.center().x-size.x*0.5f,r.y},i==0 ? esp.cop_color : IM_COL32(225,228,234,255),samples[i]);
                if(i==1) {
                    const char* rarity=tr("Legendary","Легендарный");
                    draw->AddText({r.center().x-ImGui::CalcTextSize(rarity).x*0.5f,r.y+line+2},IM_COL32(235,175,65,255),rarity);
                }
            }
            if(selected) draw->AddText({r.x,r.y-15},settings_.accent_color,names[i]);
        }
        ImGui::SetCursorScreenPos(origin);ImGui::Dummy(space);
    }
    ImGui::EndChild();
    ImGui::PushID("EspSnapControls");
    ImGui::SetNextItemWidth(155);
    ImGui::Combo(tr("Element","Элемент"),&layout_selected_,names.data(),static_cast<int>(names.size()));
    auto& selected=*elements[static_cast<std::size_t>(layout_selected_)];
    int placement=static_cast<int>(selected.anchor);
    ImGui::SetNextItemWidth(155);
    if(ImGui::Combo(tr("Placement","Позиция"),&placement,zone_names.data(),layout_selected_==4 ? 4 : 2)) {
        selected.anchor=static_cast<EspAnchor>(placement);edited=true;
    }
    bool visible=selected.visible;
    if(ImGui::Checkbox(tr("Visible","Показывать"),&visible)) {
        selected.visible=visible;edited=true;
        if(layout_selected_==0) esp.role_labels=visible;
        if(layout_selected_==2) esp.distance_labels=visible;
        if((layout_selected_==1 || layout_selected_==4) && visible) esp.weapon_labels=true;
    }
    if(ImGui::Button(tr("Reset layout","Сбросить компоновку"))) {layout={};esp.role_labels=esp.weapon_labels=esp.distance_labels=true;edited=true;}
    ImGui::PopID();
    if(edited) {dirty_=true;layout_save_pending_=true;}
    return edited;
}
