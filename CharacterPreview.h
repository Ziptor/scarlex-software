#pragma once
#include "imgui.h"
#include <iterator>

// The same shaded mannequin is used by Aimbot and the ESP layout editor.
inline void draw_character_preview(ImDrawList* draw,ImVec2 origin,float scale=1.0f) {
    const auto at=[&](float x,float y) {return ImVec2{origin.x+x*scale,origin.y+y*scale};};
    const auto joint=[&](ImVec2 a,ImVec2 b,float thickness) {
        thickness*=scale;
        draw->AddLine(a,b,IM_COL32(9,9,12,255),thickness+4*scale);
        draw->AddLine(a,b,IM_COL32(43,44,51,255),thickness);
        draw->AddCircleFilled(b,thickness*0.47f,IM_COL32(43,44,51,255));
    };
    const ImVec2 torso[]{at(53,75),at(72,66),at(100,66),at(119,75),at(107,128),
                          at(107,151),at(86,164),at(65,151),at(65,128)};
    const ImVec2 chest[]{torso[0],torso[1],torso[2],torso[3],torso[4],torso[8]};
    const ImVec2 pelvis[]{torso[8],torso[4],torso[5],torso[6],torso[7]};
    for (float side : {-1.0f,1.0f}) {
        const float shoulder=86+side*32,elbow=86+side*45,hand=86+side*57;
        const float thigh=86+side*14,knee=86+side*20,foot=86+side*23;
        joint(at(shoulder,78),at(elbow,115),14);
        joint(at(elbow,115),at(hand,153),11);
        joint(at(thigh,147),at(knee,207),21);
        joint(at(knee,207),at(foot,264),14);
        joint(at(foot,264),at(foot+side*9,271),11);
    }
    draw->AddRectFilled(at(78,49),at(94,73),IM_COL32(43,44,51,255),4*scale);
    draw->AddConvexPolyFilled(chest,static_cast<int>(std::size(chest)),IM_COL32(56,57,66,255));
    draw->AddConvexPolyFilled(pelvis,static_cast<int>(std::size(pelvis)),IM_COL32(43,44,51,255));
    draw->AddPolyline(torso,static_cast<int>(std::size(torso)),IM_COL32(10,10,13,255),ImDrawFlags_Closed,2*scale);
    draw->AddEllipseFilled(at(86,35),{18*scale,23*scale},IM_COL32(56,57,66,255));
    draw->AddEllipse(at(86,35),{18*scale,23*scale},IM_COL32(10,10,13,255),0,0,2*scale);
}
