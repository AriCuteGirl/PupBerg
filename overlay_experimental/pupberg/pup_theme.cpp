#ifdef EMU_OVERLAY

#include "pupberg/pup_theme.h"

#include <algorithm>

namespace pupberg {

static ImVec4 rgb(int r, int g, int b, float a = 1.0f)
{
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

const std::vector<Theme>& builtin_themes()
{
    static const std::vector<Theme> themes = {
        // dark with pink accent, the default
        { "Midnight Pup", true,
          rgb(4, 2, 10, 0.60f), rgb(22, 20, 32, 0.97f), rgb(16, 14, 24), rgb(32, 29, 46), rgb(42, 38, 60),
          rgb(56, 50, 78), rgb(255, 120, 180), rgb(170, 130, 255), rgb(30, 10, 24),
          rgb(240, 234, 250), rgb(150, 140, 175), rgb(120, 220, 160), rgb(250, 200, 100), rgb(255, 100, 120) },
        // warm cream + caramel
        { "Golden Retriever", false,
          rgb(46, 30, 14, 0.45f), rgb(255, 248, 236, 0.97f), rgb(250, 235, 210), rgb(255, 255, 255), rgb(255, 244, 226),
          rgb(232, 210, 178), rgb(214, 137, 50), rgb(240, 180, 90), rgb(255, 255, 255),
          rgb(74, 50, 28), rgb(150, 120, 90), rgb(98, 170, 92), rgb(232, 168, 48), rgb(212, 84, 72) },
        // cool slate + ice blue
        { "Husky", true,
          rgb(6, 12, 22, 0.55f), rgb(26, 34, 48, 0.97f), rgb(20, 27, 39), rgb(36, 46, 64), rgb(44, 56, 78),
          rgb(58, 72, 96), rgb(110, 180, 245), rgb(170, 220, 255), rgb(12, 24, 40),
          rgb(232, 240, 250), rgb(140, 160, 188), rgb(110, 210, 150), rgb(240, 196, 90), rgb(240, 110, 110) },
        // orange + white
        { "Shiba", false,
          rgb(50, 20, 0, 0.45f), rgb(255, 250, 245, 0.97f), rgb(255, 236, 218), rgb(255, 255, 255), rgb(255, 242, 230),
          rgb(244, 208, 176), rgb(232, 112, 40), rgb(255, 170, 90), rgb(255, 255, 255),
          rgb(64, 38, 24), rgb(150, 110, 90), rgb(92, 170, 100), rgb(236, 160, 40), rgb(210, 70, 60) },
        // carried over from GirlBerg
        { "Pink Neon", true,
          rgb(20, 0, 20, 0.55f), rgb(30, 12, 34, 0.97f), rgb(22, 8, 26), rgb(48, 18, 54), rgb(62, 24, 70),
          rgb(110, 40, 120), rgb(255, 64, 200), rgb(80, 220, 255), rgb(30, 0, 30),
          rgb(255, 230, 250), rgb(200, 150, 200), rgb(90, 255, 170), rgb(255, 220, 80), rgb(255, 80, 110) },
        // carried over from GirlBerg
        // the lighter dark catppuccin flavor
        { "Catppuccin Frappe", true,
          rgb(20, 22, 32, 0.45f), rgb(48, 52, 70, 0.97f), rgb(41, 44, 60), rgb(65, 69, 89), rgb(81, 87, 109),
          rgb(98, 104, 128), rgb(202, 158, 230), rgb(244, 184, 228), rgb(35, 38, 52),
          rgb(198, 208, 245), rgb(165, 173, 206), rgb(166, 209, 137), rgb(229, 200, 144), rgb(231, 130, 132) },
    };
    return themes;
}

const Theme& find_builtin_theme(const std::string &name)
{
    const auto &themes = builtin_themes();
    // renamed themes, keeps old configs/prefs working
    const std::string resolved = name == "Catppuccin Mocha" ? "Catppuccin Frappe" : name;
    for (const auto &t : themes) {
        if (t.name == resolved) return t;
    }
    return themes.front();
}

ImU32 to_u32(const ImVec4 &c, float alpha_mul)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, std::clamp(c.w * alpha_mul, 0.0f, 1.0f)));
}

ImVec4 mix(const ImVec4 &a, const ImVec4 &b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

ImVec4 with_alpha(const ImVec4 &c, float a)
{
    return ImVec4(c.x, c.y, c.z, a);
}

std::pair<int, int> push_imgui_style(const Theme &t, float scale)
{
    const std::pair<ImGuiCol, ImVec4> colors[] = {
        { ImGuiCol_Text, t.text },
        { ImGuiCol_TextDisabled, t.text_muted },
        { ImGuiCol_WindowBg, with_alpha(t.panel, 0.98f) },
        { ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0) },
        { ImGuiCol_PopupBg, with_alpha(t.card, 0.98f) },
        { ImGuiCol_Border, t.border },
        { ImGuiCol_FrameBg, mix(t.card, t.border, 0.35f) },
        { ImGuiCol_FrameBgHovered, mix(t.card, t.accent, 0.20f) },
        { ImGuiCol_FrameBgActive, mix(t.card, t.accent, 0.35f) },
        { ImGuiCol_TitleBg, t.sidebar },
        { ImGuiCol_TitleBgActive, mix(t.sidebar, t.accent, 0.25f) },
        { ImGuiCol_TitleBgCollapsed, t.sidebar },
        { ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0) },
        { ImGuiCol_ScrollbarGrab, with_alpha(t.accent, 0.45f) },
        { ImGuiCol_ScrollbarGrabHovered, with_alpha(t.accent, 0.70f) },
        { ImGuiCol_ScrollbarGrabActive, t.accent },
        { ImGuiCol_CheckMark, t.accent },
        { ImGuiCol_SliderGrab, t.accent },
        { ImGuiCol_SliderGrabActive, t.accent2 },
        { ImGuiCol_Button, t.accent },
        { ImGuiCol_ButtonHovered, mix(t.accent, t.accent2, 0.45f) },
        { ImGuiCol_ButtonActive, t.accent2 },
        { ImGuiCol_Header, with_alpha(t.accent, 0.35f) },
        { ImGuiCol_HeaderHovered, with_alpha(t.accent, 0.55f) },
        { ImGuiCol_HeaderActive, t.accent },
        { ImGuiCol_Separator, t.border },
        { ImGuiCol_ResizeGrip, with_alpha(t.accent, 0.30f) },
        { ImGuiCol_ResizeGripHovered, with_alpha(t.accent, 0.60f) },
        { ImGuiCol_ResizeGripActive, t.accent },
        { ImGuiCol_Tab, t.card },
        { ImGuiCol_TabHovered, mix(t.card, t.accent, 0.4f) },
        { ImGuiCol_TabSelected, t.accent },
        { ImGuiCol_TextSelectedBg, with_alpha(t.accent, 0.35f) },
        { ImGuiCol_PlotHistogram, t.accent },
        { ImGuiCol_ModalWindowDimBg, with_alpha(t.backdrop, 0.5f) },
    };
    for (const auto &c : colors) ImGui::PushStyleColor(c.first, c.second);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 10.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 10.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * scale, 6.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 8.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * scale, 14.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 10.0f * scale);

    return { (int)(sizeof(colors) / sizeof(colors[0])), 13 };
}

void pop_imgui_style(std::pair<int, int> counts)
{
    ImGui::PopStyleVar(counts.second);
    ImGui::PopStyleColor(counts.first);
}

}

#endif // EMU_OVERLAY
