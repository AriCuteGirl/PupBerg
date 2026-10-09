#ifndef __INCLUDED_PUP_THEME_H__
#define __INCLUDED_PUP_THEME_H__

#ifdef EMU_OVERLAY

#include <string>
#include <vector>
#include "InGameOverlay/ImGui/imgui.h"

namespace pupberg {

struct Theme
{
    std::string name{};
    bool dark = false;
    ImVec4 backdrop{};   // full screen tint behind the overlay
    ImVec4 panel{};      // main window background
    ImVec4 sidebar{};    // left navigation background
    ImVec4 card{};       // cards inside pages
    ImVec4 card_hover{};
    ImVec4 border{};
    ImVec4 accent{};     // primary color (buttons, selected tab)
    ImVec4 accent2{};    // secondary color (gradients, highlights)
    ImVec4 on_accent{};  // text drawn over the accent color
    ImVec4 text{};
    ImVec4 text_muted{};
    ImVec4 success{};
    ImVec4 warning{};
    ImVec4 danger{};
};

// name of the editable theme, its colors are stored in the PupBerg prefs
constexpr const char CUSTOM_THEME_NAME[] = "Custom";

const std::vector<Theme>& builtin_themes();
// returns the builtin theme with that name, or the first one
const Theme& find_builtin_theme(const std::string &name);

ImU32 to_u32(const ImVec4 &c, float alpha_mul = 1.0f);
ImVec4 mix(const ImVec4 &a, const ImVec4 &b, float t);
ImVec4 with_alpha(const ImVec4 &c, float a);

// pushes ImGui style colors/vars so regular ImGui windows (chat, gallery) match the theme
// returns the amount of pushed colors and vars, pass them to pop_imgui_style()
std::pair<int, int> push_imgui_style(const Theme &t, float scale);
void pop_imgui_style(std::pair<int, int> counts);

}

#endif // EMU_OVERLAY

#endif // __INCLUDED_PUP_THEME_H__
