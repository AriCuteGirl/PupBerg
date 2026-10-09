#ifdef EMU_OVERLAY

#include "pupberg/pup_ui.h"

#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <algorithm>

namespace pupberg::ui {

static constexpr float PI = 3.14159265f;

Context& ctx()
{
    static Context c{};
    return c;
}

void begin_frame(const Theme &theme, float scale)
{
    auto &c = ctx();
    c.theme = &theme;
    c.scale = scale;
    c.dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
    c.time = static_cast<float>(ImGui::GetTime());
}

float anim(ImGuiID id, bool on, float speed)
{
    auto &c = ctx();
    auto it = c.anims.find(id);
    if (it == c.anims.end()) it = c.anims.emplace(id, on ? 1.0f : 0.0f).first;
    float target = on ? 1.0f : 0.0f;
    float step = 1.0f - std::exp(-speed * c.dt);
    it->second += (target - it->second) * step;
    if (std::fabs(it->second - target) < 0.001f) it->second = target;
    return it->second;
}

static const Theme& th()
{
    return *ctx().theme;
}

static const char* label_end(const char *s)
{
    const char *p = std::strstr(s, "##");
    return p ? p : s + std::strlen(s);
}

static ImVec2 add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

struct CardState
{
    ImGuiID id{};
    ImVec2 start{};
    float width{};
    bool hoverable{};
    ImDrawList *dl{};
};
static std::vector<CardState> card_stack{};

void draw_shadow(ImDrawList *dl, ImVec2 min, ImVec2 max, float rounding, float size, float alpha)
{
    const int steps = 6;
    for (int i = steps; i >= 1; --i) {
        float f = (float)i / (float)steps;
        float e = size * f;
        float a = alpha * (1.0f - f) * (1.0f - f) * 0.9f + alpha * 0.02f;
        dl->AddRectFilled(ImVec2(min.x - e, min.y - e * 0.6f), ImVec2(max.x + e, max.y + e * 1.3f),
                          IM_COL32(0, 0, 0, (int)(a * 255.0f)), rounding + e);
    }
}

void draw_icon(ImDrawList *dl, Icon icon, ImVec2 c, float s, ImU32 col)
{
    const float lw = std::max(1.5f, s * 0.11f);
    auto P = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };

    switch (icon) {
    case Icon::None:
        break;

    case Icon::Paw:
        dl->AddEllipseFilled(P(0.0f, 0.17f), ImVec2(s * 0.25f, s * 0.21f), col);
        dl->AddCircleFilled(P(-0.31f, -0.06f), s * 0.105f, col);
        dl->AddCircleFilled(P(-0.12f, -0.29f), s * 0.11f, col);
        dl->AddCircleFilled(P(0.12f, -0.29f), s * 0.11f, col);
        dl->AddCircleFilled(P(0.31f, -0.06f), s * 0.105f, col);
        break;

    case Icon::Bone: {
        float r = s * 0.13f;
        dl->AddRectFilled(P(-0.32f, -0.09f), P(0.32f, 0.09f), col);
        dl->AddCircleFilled(P(-0.34f, -0.11f), r, col);
        dl->AddCircleFilled(P(-0.34f, 0.11f), r, col);
        dl->AddCircleFilled(P(0.34f, -0.11f), r, col);
        dl->AddCircleFilled(P(0.34f, 0.11f), r, col);
        break;
    }

    case Icon::Dog:
        dl->AddEllipseFilled(P(-0.33f, -0.02f), ImVec2(s * 0.14f, s * 0.28f), col, 0.35f);
        dl->AddEllipseFilled(P(0.33f, -0.02f), ImVec2(s * 0.14f, s * 0.28f), col, -0.35f);
        dl->AddCircle(P(0.0f, 0.02f), s * 0.3f, col, 0, lw);
        dl->AddCircleFilled(P(-0.11f, -0.04f), s * 0.05f, col);
        dl->AddCircleFilled(P(0.11f, -0.04f), s * 0.05f, col);
        dl->AddEllipseFilled(P(0.0f, 0.12f), ImVec2(s * 0.08f, s * 0.055f), col);
        break;

    case Icon::Home:
        dl->AddTriangleFilled(P(-0.42f, -0.02f), P(0.0f, -0.42f), P(0.42f, -0.02f), col);
        dl->AddRectFilled(P(-0.3f, -0.05f), P(0.3f, 0.38f), col, s * 0.05f);
        dl->AddRectFilled(P(-0.08f, 0.12f), P(0.08f, 0.38f), IM_COL32(0, 0, 0, 90), s * 0.03f);
        break;

    case Icon::Friends:
        dl->AddCircleFilled(P(-0.16f, -0.18f), s * 0.15f, col);
        dl->AddRectFilled(P(-0.42f, 0.03f), P(0.1f, 0.38f), col, s * 0.2f, ImDrawFlags_RoundCornersTop);
        dl->AddCircleFilled(P(0.22f, -0.12f), s * 0.12f, col);
        dl->AddRectFilled(P(0.05f, 0.06f), P(0.44f, 0.38f), col, s * 0.16f, ImDrawFlags_RoundCornersTop);
        break;

    case Icon::Trophy:
        dl->AddRectFilled(P(-0.25f, -0.38f), P(0.25f, 0.05f), col, s * 0.22f, ImDrawFlags_RoundCornersBottom);
        dl->AddCircle(P(-0.27f, -0.2f), s * 0.11f, col, 0, lw);
        dl->AddCircle(P(0.27f, -0.2f), s * 0.11f, col, 0, lw);
        dl->AddRectFilled(P(-0.05f, 0.0f), P(0.05f, 0.22f), col);
        dl->AddRectFilled(P(-0.22f, 0.22f), P(0.22f, 0.36f), col, s * 0.04f);
        break;

    case Icon::Network:
        dl->AddCircleFilled(P(0.0f, 0.28f), s * 0.08f, col);
        for (int i = 1; i <= 3; ++i) {
            float r = s * 0.15f * i;
            dl->PathArcTo(P(0.0f, 0.28f), r, PI * 1.25f, PI * 1.75f, 12);
            dl->PathStroke(col, 0, lw);
        }
        break;

    case Icon::Camera:
        dl->AddRectFilled(P(-0.42f, -0.22f), P(0.42f, 0.32f), col, s * 0.1f);
        dl->AddRectFilled(P(-0.15f, -0.34f), P(0.15f, -0.2f), col, s * 0.04f);
        dl->AddCircleFilled(P(0.0f, 0.05f), s * 0.17f, IM_COL32(0, 0, 0, 110));
        dl->AddCircle(P(0.0f, 0.05f), s * 0.17f, IM_COL32(255, 255, 255, 160), 0, lw * 0.7f);
        break;

    case Icon::Gear: {
        for (int i = 0; i < 8; ++i) {
            float a = i * PI / 4.0f;
            ImVec2 d(std::cos(a), std::sin(a));
            ImVec2 n(-d.y, d.x);
            float r0 = s * 0.22f, r1 = s * 0.42f, w = s * 0.09f;
            dl->AddQuadFilled(
                ImVec2(c.x + d.x * r0 - n.x * w, c.y + d.y * r0 - n.y * w),
                ImVec2(c.x + d.x * r1 - n.x * w, c.y + d.y * r1 - n.y * w),
                ImVec2(c.x + d.x * r1 + n.x * w, c.y + d.y * r1 + n.y * w),
                ImVec2(c.x + d.x * r0 + n.x * w, c.y + d.y * r0 + n.y * w), col);
        }
        dl->AddCircle(c, s * 0.22f, col, 0, s * 0.16f);
        break;
    }

    case Icon::Chat:
        dl->AddRectFilled(P(-0.42f, -0.34f), P(0.42f, 0.2f), col, s * 0.14f);
        dl->AddTriangleFilled(P(-0.22f, 0.18f), P(0.0f, 0.18f), P(-0.26f, 0.4f), col);
        break;

    case Icon::Close:
        dl->AddLine(P(-0.28f, -0.28f), P(0.28f, 0.28f), col, lw * 1.2f);
        dl->AddLine(P(0.28f, -0.28f), P(-0.28f, 0.28f), col, lw * 1.2f);
        break;

    case Icon::Copy:
        dl->AddRect(P(-0.3f, -0.36f), P(0.16f, 0.16f), col, s * 0.08f, 0, lw);
        dl->AddRectFilled(P(-0.12f, -0.16f), P(0.34f, 0.36f), col, s * 0.08f);
        break;

    case Icon::Plus:
        dl->AddLine(P(-0.32f, 0.0f), P(0.32f, 0.0f), col, lw * 1.4f);
        dl->AddLine(P(0.0f, -0.32f), P(0.0f, 0.32f), col, lw * 1.4f);
        break;

    case Icon::Bell:
        dl->AddEllipseFilled(P(0.0f, -0.08f), ImVec2(s * 0.26f, s * 0.28f), col);
        dl->AddRectFilled(P(-0.26f, -0.08f), P(0.26f, 0.22f), col);
        dl->AddRectFilled(P(-0.36f, 0.18f), P(0.36f, 0.27f), col, s * 0.04f);
        dl->AddCircleFilled(P(0.0f, 0.36f), s * 0.08f, col);
        break;

    case Icon::Lock:
        dl->PathArcTo(P(0.0f, -0.08f), s * 0.2f, PI, PI * 2.0f, 12);
        dl->PathStroke(col, 0, lw * 1.2f);
        dl->AddLine(P(-0.2f, -0.08f), P(-0.2f, 0.02f), col, lw * 1.2f);
        dl->AddLine(P(0.2f, -0.08f), P(0.2f, 0.02f), col, lw * 1.2f);
        dl->AddRectFilled(P(-0.32f, 0.0f), P(0.32f, 0.4f), col, s * 0.08f);
        break;

    case Icon::Check: {
        ImVec2 pts[] = { P(-0.32f, 0.02f), P(-0.08f, 0.26f), P(0.34f, -0.22f) };
        dl->AddPolyline(pts, 3, col, 0, lw * 1.5f);
        break;
    }
    }
}

void draw_dog(ImDrawList *dl, ImVec2 c, float r, const Theme &t, float wag)
{
    ImVec4 fur = mix(t.accent, ImVec4(1, 1, 1, 1), 0.25f);
    ImVec4 ear = mix(t.accent, ImVec4(0, 0, 0, 1), 0.25f);
    ImVec4 muzzle = mix(fur, ImVec4(1, 1, 1, 1), 0.65f);
    ImU32 dark = IM_COL32(40, 28, 24, 255);
    float ear_rot = 0.35f + std::sin(wag) * 0.08f;

    dl->AddCircleFilled(c, r, to_u32(fur), 40);
    // floppy ears
    dl->AddEllipseFilled(ImVec2(c.x - r * 0.82f, c.y + r * 0.05f), ImVec2(r * 0.34f, r * 0.68f), to_u32(ear), ear_rot, 24);
    dl->AddEllipseFilled(ImVec2(c.x + r * 0.82f, c.y + r * 0.05f), ImVec2(r * 0.34f, r * 0.68f), to_u32(ear), -ear_rot, 24);
    // muzzle + tongue
    dl->AddEllipseFilled(ImVec2(c.x, c.y + r * 0.62f), ImVec2(r * 0.16f, r * 0.2f), IM_COL32(240, 110, 130, 255), 0.0f, 16);
    dl->AddEllipseFilled(ImVec2(c.x, c.y + r * 0.38f), ImVec2(r * 0.5f, r * 0.34f), to_u32(muzzle), 0.0f, 24);
    // eyes
    for (float side : { -1.0f, 1.0f }) {
        ImVec2 e(c.x + side * r * 0.36f, c.y - r * 0.08f);
        dl->AddCircleFilled(e, r * 0.12f, dark, 16);
        dl->AddCircleFilled(ImVec2(e.x + r * 0.04f, e.y - r * 0.04f), r * 0.04f, IM_COL32(255, 255, 255, 230), 8);
    }
    // nose
    dl->AddEllipseFilled(ImVec2(c.x, c.y + r * 0.22f), ImVec2(r * 0.16f, r * 0.11f), dark, 0.0f, 16);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.05f, c.y + r * 0.19f), r * 0.035f, IM_COL32(255, 255, 255, 140), 8);
}

void draw_paw_pattern(ImDrawList *dl, ImVec2 min, ImVec2 max, ImU32 col, float spacing)
{
    int row = 0;
    for (float y = min.y + spacing * 0.5f; y < max.y + spacing; y += spacing * 0.8f, ++row) {
        float off = (row % 2) ? spacing * 0.5f : 0.0f;
        for (float x = min.x + off; x < max.x + spacing; x += spacing) {
            draw_icon(dl, Icon::Paw, ImVec2(x, y), spacing * 0.28f, col);
        }
    }
}

void draw_avatar(ImDrawList *dl, ImVec2 c, float r, const std::string &name, bool online)
{
    unsigned hash = 2166136261u;
    for (unsigned char ch : name) { hash ^= ch; hash *= 16777619u; }
    float red, green, blue;
    ImGui::ColorConvertHSVtoRGB((hash % 360) / 360.0f, 0.45f, 0.85f, red, green, blue);
    dl->AddCircleFilled(c, r, ImGui::ColorConvertFloat4ToU32(ImVec4(red, green, blue, 1.0f)), 32);
    draw_icon(dl, Icon::Paw, ImVec2(c.x, c.y + r * 0.05f), r * 1.1f, IM_COL32(255, 255, 255, 70));

    char initial[2] = { name.empty() ? '?' : (char)std::toupper((unsigned char)name[0]), 0 };
    ImVec2 ts = ImGui::CalcTextSize(initial);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), initial);

    if (online) {
        ImVec2 d(c.x + r * 0.72f, c.y + r * 0.72f);
        dl->AddCircleFilled(d, r * 0.3f, to_u32(th().card), 16);
        dl->AddCircleFilled(d, r * 0.2f, to_u32(th().success), 16);
    }
}

bool button(const char *id_label, ImVec2 size, ButtonKind kind, Icon icon, bool enabled)
{
    const Theme &t = th();
    const char *end = label_end(id_label);
    ImVec2 ts = ImGui::CalcTextSize(id_label, end);
    float fs = ImGui::GetFontSize();
    float pad = S(14.0f);
    float icon_w = icon != Icon::None ? fs + (ts.x > 0 ? S(6.0f) : 0.0f) : 0.0f;

    float w = size.x > 0 ? size.x : (size.x < 0 ? ImGui::GetContentRegionAvail().x : ts.x + icon_w + pad * 2.0f);
    float h = size.y > 0 ? size.y : fs + S(14.0f);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id_label, ImVec2(w, h));
    bool hovered = enabled && ImGui::IsItemHovered();
    bool held = enabled && ImGui::IsItemActive();
    float ha = anim(ImGui::GetItemID(), hovered);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImVec4 bg{}, fg{};
    switch (kind) {
    case ButtonKind::Primary: bg = mix(t.accent, t.accent2, ha * 0.35f); fg = t.on_accent; break;
    case ButtonKind::Soft:    bg = mix(mix(t.card, t.accent, 0.14f), t.accent, ha * 0.18f); fg = t.text; break;
    case ButtonKind::Ghost:   bg = with_alpha(t.accent, ha * 0.16f); fg = t.text; break;
    case ButtonKind::Danger:  bg = mix(t.danger, ImVec4(1, 1, 1, 1), ha * 0.15f); fg = ImVec4(1, 1, 1, 1); break;
    }
    if (!enabled) { bg = with_alpha(bg, bg.w * 0.4f); fg = with_alpha(fg, 0.45f); }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    float lift = held ? S(1.0f) : -ha * S(1.0f);
    ImVec2 a(pos.x, pos.y + lift), b(pos.x + w, pos.y + h + lift);
    if (kind == ButtonKind::Primary && enabled) {
        dl->AddRectFilled(ImVec2(a.x, a.y + S(3.0f)), ImVec2(b.x, b.y + S(3.0f)), to_u32(mix(t.accent, ImVec4(0, 0, 0, 1), 0.3f), 0.5f), h * 0.5f);
    }
    dl->AddRectFilled(a, b, to_u32(bg), h * 0.5f);

    float content_w = ts.x + icon_w;
    float x = a.x + (w - content_w) * 0.5f;
    float cy = a.y + h * 0.5f;
    if (icon != Icon::None) {
        draw_icon(dl, icon, ImVec2(x + fs * 0.5f, cy), fs * 0.95f, to_u32(fg));
        x += icon_w;
    }
    if (ts.x > 0) dl->AddText(ImVec2(x, cy - ts.y * 0.5f), to_u32(fg), id_label, end);

    return pressed && enabled;
}

bool icon_button(const char *id, Icon icon, float size, const char *tooltip)
{
    const Theme &t = th();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    bool hovered = ImGui::IsItemHovered();
    float ha = anim(ImGui::GetItemID(), hovered);
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (tooltip) ImGui::SetTooltip("%s", tooltip);
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
    dl->AddCircleFilled(c, size * 0.5f, to_u32(t.accent, 0.12f + ha * 0.2f), 24);
    draw_icon(dl, icon, c, size * 0.5f, to_u32(mix(t.text_muted, t.text, ha)));
    return pressed;
}

bool nav_item(const char *id_label, Icon icon, bool selected, float width)
{
    const Theme &t = th();
    const char *end = label_end(id_label);
    float h = S(42.0f);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id_label, ImVec2(width, h));
    bool hovered = ImGui::IsItemHovered();
    ImGuiID id = ImGui::GetItemID();
    float ha = anim(id, hovered);
    float sa = anim(id + 1, selected, 10.0f);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = pos, b(pos.x + width, pos.y + h);
    dl->AddRectFilled(a, b, to_u32(t.accent, ha * 0.10f + sa * 0.22f), S(12.0f));
    if (sa > 0.01f) {
        float bar_h = h * 0.55f * sa;
        dl->AddRectFilled(ImVec2(a.x + S(4.0f), a.y + (h - bar_h) * 0.5f), ImVec2(a.x + S(8.0f), a.y + (h + bar_h) * 0.5f), to_u32(t.accent), S(2.0f));
    }

    ImVec4 fg = mix(t.text_muted, t.text, std::max(ha, sa));
    float fs = ImGui::GetFontSize();
    float ix = a.x + S(26.0f) + ha * S(2.0f);
    draw_icon(dl, icon, ImVec2(ix, a.y + h * 0.5f), fs * 1.05f, to_u32(sa > 0.5f ? t.accent : fg));
    ImVec2 ts = ImGui::CalcTextSize(id_label, end);
    dl->AddText(ImVec2(ix + fs + S(6.0f), a.y + (h - ts.y) * 0.5f), to_u32(fg), id_label, end);
    return pressed;
}

bool toggle(const char *id_label, bool *value)
{
    const Theme &t = th();
    const char *end = label_end(id_label);
    float h = ImGui::GetFontSize() + S(6.0f);
    float w = h * 1.8f;
    ImVec2 ts = ImGui::CalcTextSize(id_label, end);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id_label, ImVec2(w + (ts.x > 0 ? S(10.0f) + ts.x : 0.0f), h));
    if (pressed) *value = !*value;
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    float on = anim(ImGui::GetItemID(), *value, 14.0f);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), to_u32(mix(mix(t.card, t.border, 0.8f), t.accent, on)), h * 0.5f);
    float kr = h * 0.5f - S(3.0f);
    ImVec2 kc(pos.x + h * 0.5f + (w - h) * on, pos.y + h * 0.5f);
    dl->AddCircleFilled(ImVec2(kc.x, kc.y + S(1.0f)), kr, IM_COL32(0, 0, 0, 50), 20);
    dl->AddCircleFilled(kc, kr, IM_COL32(255, 255, 255, 255), 20);
    if (on > 0.5f) draw_icon(dl, Icon::Paw, kc, kr * 1.2f, to_u32(t.accent, (on - 0.5f) * 2.0f));
    if (ts.x > 0) dl->AddText(ImVec2(pos.x + w + S(10.0f), pos.y + (h - ts.y) * 0.5f), to_u32(t.text), id_label, end);
    return pressed;
}

bool segmented(const char *id, const char *const *items, int count, int *selected)
{
    const Theme &t = th();
    ImGui::PushID(id);
    float h = ImGui::GetFontSize() + S(12.0f);
    float pad = S(16.0f);
    std::vector<float> widths(count);
    float total = S(6.0f);
    for (int i = 0; i < count; ++i) { widths[i] = ImGui::CalcTextSize(items[i]).x + pad * 2.0f; total += widths[i]; }

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + total, pos.y + h + S(6.0f)), to_u32(mix(t.card, t.border, 0.5f)), (h + S(6.0f)) * 0.5f);

    bool changed = false;
    float x = pos.x + S(3.0f);
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(x, pos.y + S(3.0f)));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(widths[i], h)) && *selected != i) { *selected = i; changed = true; }
        bool hovered = ImGui::IsItemHovered();
        float sa = anim(ImGui::GetItemID(), *selected == i, 14.0f);
        ImGui::PopID();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (sa > 0.01f) dl->AddRectFilled(ImVec2(x, pos.y + S(3.0f)), ImVec2(x + widths[i], pos.y + S(3.0f) + h), to_u32(t.accent, sa), h * 0.5f);
        ImVec2 ts = ImGui::CalcTextSize(items[i]);
        dl->AddText(ImVec2(x + (widths[i] - ts.x) * 0.5f, pos.y + S(3.0f) + (h - ts.y) * 0.5f), to_u32(mix(t.text, t.on_accent, sa)), items[i]);
        x += widths[i];
    }
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(total, h + S(6.0f)));
    ImGui::PopID();
    return changed;
}

void chip(const char *text, const ImVec4 &color)
{
    ImVec2 ts = ImGui::CalcTextSize(text);
    float h = ts.y + S(6.0f);
    float dot = S(7.0f);
    ImVec2 size(ts.x + S(22.0f) + dot, h);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + h), to_u32(color, 0.18f), h * 0.5f);
    dl->AddCircleFilled(ImVec2(pos.x + S(10.0f), pos.y + h * 0.5f), dot * 0.5f, to_u32(color), 12);
    dl->AddText(ImVec2(pos.x + S(14.0f) + dot * 0.5f, pos.y + S(3.0f)), to_u32(mix(color, th().text, 0.35f)), text);
    ImGui::Dummy(size);
}

void bone_progress(float fraction, ImVec2 size, const char *overlay_text)
{
    const Theme &t = th();
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (size.x <= 0) size.x = card_stack.empty() ? ImGui::GetContentRegionAvail().x : card_inner_right() - ImGui::GetCursorScreenPos().x;
    if (size.y <= 0) size.y = S(12.0f);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size.x, size.y + S(6.0f)));

    ImDrawList *dl = ImGui::GetWindowDrawList();
    float h = size.y;
    float kr = h * 0.52f;
    float cy = pos.y + S(3.0f) + h * 0.5f;
    float x0 = pos.x + kr * 1.4f, x1 = pos.x + size.x - kr * 1.4f;
    ImU32 track = to_u32(mix(t.card, t.border, 0.9f));
    ImU32 fill = to_u32(t.accent);

    auto knobs = [&](float x, ImU32 col) {
        dl->AddCircleFilled(ImVec2(x, cy - h * 0.32f), kr, col, 16);
        dl->AddCircleFilled(ImVec2(x, cy + h * 0.32f), kr, col, 16);
    };

    dl->AddRectFilled(ImVec2(x0, cy - h * 0.36f), ImVec2(x1, cy + h * 0.36f), track, h * 0.3f);
    knobs(x0, fraction > 0.0f ? fill : track);
    knobs(x1, fraction >= 1.0f ? fill : track);
    if (fraction > 0.0f) {
        float fx = x0 + (x1 - x0) * fraction;
        dl->AddRectFilled(ImVec2(x0, cy - h * 0.36f), ImVec2(fx, cy + h * 0.36f), fill, h * 0.3f);
    }
    if (overlay_text) {
        ImVec2 ts = ImGui::CalcTextSize(overlay_text);
        dl->AddText(ImVec2(pos.x + (size.x - ts.x) * 0.5f, cy + h * 0.5f + S(4.0f)), to_u32(t.text_muted), overlay_text);
        ImGui::Dummy(ImVec2(1, ts.y));
    }
}

void heading(const char *text, float size_mul)
{
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * size_mul);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void text_muted(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, th().text_muted);
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void spacer(float h)
{
    ImGui::Dummy(ImVec2(1, S(h)));
}



void begin_card(const char *id, float width, bool hoverable)
{
    CardState cs{};
    cs.dl = ImGui::GetWindowDrawList();
    cs.start = ImGui::GetCursorScreenPos();
    cs.width = width > 0 ? width : ImGui::GetContentRegionAvail().x;
    cs.hoverable = hoverable;
    ImGui::PushID(id);
    cs.id = ImGui::GetID("##card");
    card_stack.push_back(cs);

    cs.dl->ChannelsSplit(2);
    cs.dl->ChannelsSetCurrent(1);
    float pad = S(16.0f);
    ImGui::SetCursorScreenPos(ImVec2(cs.start.x + pad, cs.start.y + pad));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + cs.width - pad * 2.0f);
}

float card_inner_right()
{
    if (card_stack.empty()) return ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const CardState &cs = card_stack.back();
    return cs.start.x + cs.width - S(16.0f);
}

bool end_card()
{
    const Theme &t = th();
    CardState cs = card_stack.back();
    card_stack.pop_back();
    float pad = S(16.0f);

    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImVec2 inner_max = ImGui::GetItemRectMax();
    ImVec2 min = cs.start;
    ImVec2 max(cs.start.x + cs.width, inner_max.y + pad);

    bool hovered = cs.hoverable && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(min, max);
    bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered();
    float ha = cs.hoverable ? anim(cs.id, hovered) : 0.0f;
    if (hovered && !ImGui::IsAnyItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    cs.dl->ChannelsSetCurrent(0);
    float rounding = S(16.0f);
    float lift = ha * S(2.0f);
    ImVec2 a(min.x, min.y - lift), b(max.x, max.y - lift);
    draw_shadow(cs.dl, a, b, rounding, S(8.0f) + ha * S(4.0f), t.dark ? 0.35f : 0.12f);
    cs.dl->AddRectFilled(a, b, to_u32(mix(t.card, t.card_hover, ha)), rounding);
    cs.dl->AddRect(a, b, to_u32(mix(t.border, t.accent, ha * 0.6f), t.dark ? 0.6f : 0.8f), rounding, 0, 1.0f);
    cs.dl->ChannelsMerge();

    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy(ImVec2(cs.width, max.y - min.y));
    ImGui::PopID();
    return clicked;
}

}

#endif // EMU_OVERLAY
