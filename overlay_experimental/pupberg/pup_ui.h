#ifndef __INCLUDED_PUP_UI_H__
#define __INCLUDED_PUP_UI_H__

#ifdef EMU_OVERLAY

#include <string>
#include <unordered_map>
#include "InGameOverlay/ImGui/imgui.h"
#include "pupberg/pup_theme.h"

// small immediate mode widget kit drawn with ImDrawList on top of ImGui's layout/input,
// successor of GirlBerg's custom_ui_core
namespace pupberg::ui {

enum class Icon
{
    None, Paw, Bone, Dog, Home, Friends, Trophy, Network, Camera, Gear, Chat, Close, Copy, Plus, Bell, Lock, Check,
};

enum class ButtonKind
{
    Primary, // filled with accent
    Soft,    // tinted card
    Ghost,   // transparent until hovered
    Danger,
};

// per frame state shared by all widgets
struct Context
{
    const Theme *theme{};
    float scale = 1.0f;
    float dt = 0.016f;
    float time = 0.0f;
    std::unordered_map<ImGuiID, float> anims{};
};

Context& ctx();
void begin_frame(const Theme &theme, float scale);

// 0..1 value easing towards `on` for widget `id`
float anim(ImGuiID id, bool on, float speed = 12.0f);
inline float S(float v) { return v * ctx().scale; }

void draw_shadow(ImDrawList *dl, ImVec2 min, ImVec2 max, float rounding, float size, float alpha);
void draw_icon(ImDrawList *dl, Icon icon, ImVec2 center, float size, ImU32 col);
// the PupBerg mascot: a round dog face with floppy ears
void draw_dog(ImDrawList *dl, ImVec2 center, float radius, const Theme &t, float wag = 0.0f);
// decorative paw prints scattered over a rect
void draw_paw_pattern(ImDrawList *dl, ImVec2 min, ImVec2 max, ImU32 col, float spacing);
void draw_avatar(ImDrawList *dl, ImVec2 center, float radius, const std::string &name, bool online);

bool button(const char *id_label, ImVec2 size = ImVec2(0, 0), ButtonKind kind = ButtonKind::Primary, Icon icon = Icon::None, bool enabled = true);
bool icon_button(const char *id, Icon icon, float size, const char *tooltip = nullptr);
bool nav_item(const char *id_label, Icon icon, bool selected, float width);
bool toggle(const char *id_label, bool *value);
// segmented control, returns true when the selection changed
bool segmented(const char *id, const char *const *items, int count, int *selected);
void chip(const char *text, const ImVec4 &color);
// a bone shaped progress bar
void bone_progress(float fraction, ImVec2 size, const char *overlay_text = nullptr);

void heading(const char *text, float size_mul = 1.6f);
void text_muted(const char *fmt, ...);
void spacer(float h);

// cards: everything between begin_card()/end_card() gets a rounded background with a shadow
void begin_card(const char *id, float width = 0.0f, bool hoverable = false);
// returns true if the card was clicked (only when hoverable)
bool end_card();
// screen x of the right edge of the current card content, for right aligned buttons
float card_inner_right();

}

#endif // EMU_OVERLAY

#endif // __INCLUDED_PUP_UI_H__
