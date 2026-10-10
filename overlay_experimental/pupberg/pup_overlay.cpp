#ifdef EMU_OVERLAY

#include "pupberg/pup_overlay.h"

#include "overlay/steam_overlay.h"
// BringWindowToDisplayFront() for the invite popup; an earlier header defines BLOCK_SIZE, a template parameter name in there
#pragma push_macro("BLOCK_SIZE")
#undef BLOCK_SIZE
#include "InGameOverlay/ImGui/imgui_internal.h"
#pragma pop_macro("BLOCK_SIZE")
#include "dll/dll.h"

#include "pupberg/pup_ui.h"
#include "pupberg/pup_zerotier.h"
#include "pupberg/pup_input.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <random>
#include <ctime>

using namespace pupberg;
using ui::Icon;
using ui::ButtonKind;
using ui::S;

static constexpr const char PREFS_FILE[] = "pupberg_prefs.json";
static constexpr const char PUPBERG_VERSION[] = "0.1.0";

static float ease_out_cubic(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    float f = 1.0f - t;
    return 1.0f - f * f * f;
}

static float seconds_since(std::chrono::steady_clock::time_point tp)
{
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - tp).count();
}

static bool contains_insensitive(const std::string &haystack, const char *needle)
{
    if (!needle || !needle[0]) return true;
    std::string h(haystack), n(needle);
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return h.find(n) != std::string::npos;
}

static nlohmann::json color_to_json(const ImVec4 &c)
{
    return nlohmann::json::array({ c.x, c.y, c.z, c.w });
}

static void color_from_json(const nlohmann::json &j, const char *key, ImVec4 &out)
{
    if (!j.contains(key) || !j[key].is_array() || j[key].size() != 4) return;
    try {
        out = ImVec4(j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>(), j[key][3].get<float>());
    } catch (...) {}
}

// ---------------------------------------------------------------------------

PupOverlay::PupOverlay(Steam_Overlay &overlay) :
    ov(overlay)
{
    const auto &pup = ov.settings->pupberg;
    theme_name = pup.theme;
    ui_scale = pup.ui_scale;
    zt_auto_join = pup.zerotier_auto_join;
    std::strncpy(zt_network_input, pup.zerotier_network.c_str(), sizeof(zt_network_input) - 1);
    custom_theme = find_builtin_theme(theme_name);
    custom_theme.name = CUSTOM_THEME_NAME;
    server_mode = pup.server_mode;
    std::strncpy(lobby_server_input, pup.lobby_server.c_str(), sizeof(lobby_server_input) - 1);
    std::strncpy(lobby_room_input, pup.lobby_room.c_str(), sizeof(lobby_room_input) - 1);
    lobby_public = pup.lobby_public;
    lobby_auto_join = pup.lobby_auto_join;

    load_prefs();
    select_theme(theme_name);

    input = std::make_unique<InputFallback>(ov.toggle_keys, pup.home_key_toggle, [this] { ov.toggle_overlay_deduped(false); });

    zt = std::make_unique<ZeroTierClient>(pup.zerotier_api, pup.zerotier_token, pup.zerotier_token_path);
    // send LAN discovery packets to the broadcast address of every ZeroTier subnet we're in
    zt->set_address_callback([this](uint32_t ip, int prefix) {
        uint32_t mask = prefix <= 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
        add_broadcast(ip | ~mask);
    });

    for (const auto &peer : peer_ips) {
        uint32_t ip{};
        if (ZeroTierClient::parse_ipv4(peer, ip)) add_broadcast(ip);
    }

    if (zt_auto_join && ZeroTierClient::valid_network_id(zt_network_input)) {
        zt->join(zt_network_input);
        zt_auto_join_done = true;
    }

    // the prefs saved by this overlay win over configs.overlay.ini (Steam_Client already joined from that)
    if (server_mode && lobby_auto_join && valid_room_code(lobby_room_input)) {
        join_room(lobby_room_input);
    } else if (!server_mode) {
        ov.network->relay_leave();
    }
}

PupOverlay::~PupOverlay()
{
    // stop the input poller first, its callback uses the overlay
    input.reset();
}

void PupOverlay::add_broadcast(uint32_t ip)
{
    {
        std::lock_guard lock(broadcasts_mutex);
        if (!added_broadcasts.insert(ip).second) return;
    }
    PRINT_DEBUG("PupBerg adding broadcast target %s", ZeroTierClient::ipv4_to_string(ip).c_str());
    ov.network->add_custom_broadcast(ip);
}

void PupOverlay::load_prefs()
{
    std::vector<char> buf(64 * 1024);
    int read = ov.local_storage->get_data_settings(PREFS_FILE, buf.data(), static_cast<unsigned int>(buf.size() - 1));
    if (read <= 0) return;

    try {
        auto j = nlohmann::json::parse(std::string(buf.data(), read));
        theme_name = j.value("theme", theme_name);
        ui_scale = std::clamp(j.value("ui_scale", ui_scale), 0.5f, 3.0f);
        if (j.contains("classic_frontend")) {
            bool classic = j.value("classic_frontend", false);
            // this runs inside the Steam_Client constructor, calling get_steam_client() here
            // would construct another client and recurse until the stack overflows
            ov.settings->pupberg.classic_frontend = classic;
        }
        zt_auto_join = j.value("zerotier_auto_join", zt_auto_join);
        std::string net = j.value("zerotier_network", std::string(zt_network_input));
        std::memset(zt_network_input, 0, sizeof(zt_network_input));
        std::strncpy(zt_network_input, net.c_str(), sizeof(zt_network_input) - 1);
        server_mode = j.value("network_mode", std::string(server_mode ? "server" : "zerotier")) == "server";
        std::string server = j.value("lobby_server", std::string(lobby_server_input));
        std::memset(lobby_server_input, 0, sizeof(lobby_server_input));
        std::strncpy(lobby_server_input, server.c_str(), sizeof(lobby_server_input) - 1);
        std::string room = j.value("lobby_room", std::string(lobby_room_input));
        std::memset(lobby_room_input, 0, sizeof(lobby_room_input));
        std::strncpy(lobby_room_input, room.c_str(), sizeof(lobby_room_input) - 1);
        lobby_public = j.value("lobby_public", lobby_public);
        lobby_auto_join = j.value("lobby_auto_join", lobby_auto_join);
        if (j.contains("window_offset") && j["window_offset"].is_array() && j["window_offset"].size() == 2) {
            win_offset = ImVec2(j["window_offset"][0].get<float>(), j["window_offset"][1].get<float>());
        }
        if (j.contains("peers") && j["peers"].is_array()) {
            peer_ips.clear();
            for (const auto &p : j["peers"]) if (p.is_string()) peer_ips.push_back(p.get<std::string>());
        }
        if (j.contains("custom_theme") && j["custom_theme"].is_object()) {
            const auto &c = j["custom_theme"];
            custom_theme.dark = c.value("dark", custom_theme.dark);
            color_from_json(c, "backdrop", custom_theme.backdrop);
            color_from_json(c, "panel", custom_theme.panel);
            color_from_json(c, "sidebar", custom_theme.sidebar);
            color_from_json(c, "card", custom_theme.card);
            color_from_json(c, "card_hover", custom_theme.card_hover);
            color_from_json(c, "border", custom_theme.border);
            color_from_json(c, "accent", custom_theme.accent);
            color_from_json(c, "accent2", custom_theme.accent2);
            color_from_json(c, "on_accent", custom_theme.on_accent);
            color_from_json(c, "text", custom_theme.text);
            color_from_json(c, "text_muted", custom_theme.text_muted);
        }
    } catch (...) {
        PRINT_DEBUG("PupBerg failed to parse %s", PREFS_FILE);
    }
}

void PupOverlay::save_prefs()
{
    nlohmann::json j{};
    j["theme"] = theme_name;
    j["ui_scale"] = ui_scale;
    j["classic_frontend"] = ov.settings->pupberg.classic_frontend;
    j["zerotier_network"] = std::string(zt_network_input);
    j["zerotier_auto_join"] = zt_auto_join;
    j["peers"] = peer_ips;
    j["network_mode"] = server_mode ? "server" : "zerotier";
    j["lobby_server"] = std::string(lobby_server_input);
    j["lobby_room"] = std::string(lobby_room_input);
    j["lobby_public"] = lobby_public;
    j["lobby_auto_join"] = lobby_auto_join;
    j["window_offset"] = { win_offset.x, win_offset.y };
    j["custom_theme"] = {
        { "dark", custom_theme.dark },
        { "backdrop", color_to_json(custom_theme.backdrop) },
        { "panel", color_to_json(custom_theme.panel) },
        { "sidebar", color_to_json(custom_theme.sidebar) },
        { "card", color_to_json(custom_theme.card) },
        { "card_hover", color_to_json(custom_theme.card_hover) },
        { "border", color_to_json(custom_theme.border) },
        { "accent", color_to_json(custom_theme.accent) },
        { "accent2", color_to_json(custom_theme.accent2) },
        { "on_accent", color_to_json(custom_theme.on_accent) },
        { "text", color_to_json(custom_theme.text) },
        { "text_muted", color_to_json(custom_theme.text_muted) },
    };
    std::string data = j.dump(2);
    ov.local_storage->store_data_settings(PREFS_FILE, data.c_str(), static_cast<unsigned int>(data.size()));
}

void PupOverlay::select_theme(const std::string &name)
{
    theme_name = name;
    if (name == CUSTOM_THEME_NAME) {
        theme = custom_theme;
    } else {
        theme = find_builtin_theme(name);
        theme_name = theme.name;
    }
}

void PupOverlay::set_page(Page p)
{
    if (page == p) return;
    page = p;
    page_switch_time = std::chrono::steady_clock::now();
}

std::string PupOverlay::key_combo_text() const
{
    static const char *names[] = { "Shift", "Ctrl", "Alt", "Tab", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
    std::string out{};
    for (auto k : ov.toggle_keys) {
        size_t idx = static_cast<size_t>(k);
        if (!out.empty()) out += " + ";
        out += idx < sizeof(names) / sizeof(names[0]) ? names[idx] : "?";
    }
    return out.empty() ? std::string("Shift + Tab") : out;
}

// ---------------------------------------------------------------------------

void PupOverlay::tick(bool overlay_shown)
{
    if (overlay_shown && !was_shown) {
        open_time = std::chrono::steady_clock::now();
        page_switch_time = open_time;
    }
    if (overlay_shown != was_shown) zt->set_fast_polling(overlay_shown);
    was_shown = overlay_shown;

    if (overlay_shown) {
        // the hook never delivered the toggle combo, so it most likely doesn't see any input either
        if (!ov.hook_toggle_seen) input->feed_imgui();
        input->set_typing(ImGui::GetIO().WantTextInput);
    } else {
        input->set_typing(false);
    }
}

void PupOverlay::render_classic_switch()
{
    ImGuiIO &io = ImGui::GetIO();
    ui::begin_frame(theme, 1.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16.0f, io.DisplaySize.y - 16.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##pupberg_classic_switch", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
        if (ui::button("PupBerg overlay##switch_back", ImVec2(0, 0), ButtonKind::Primary, Icon::Paw)) {
            ov.settings->pupberg.classic_frontend = false;
            save_prefs();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PupOverlay::render()
{
    ImGuiIO &io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    if (W <= 0 || H <= 0) return;

    scale = ui_scale * std::clamp(H / 1080.0f, 0.8f, 2.0f);
    if (theme_name == CUSTOM_THEME_NAME) theme = custom_theme;
    ui::begin_frame(theme, scale);

    const float open_t = ease_out_cubic(seconds_since(open_time) / 0.25f);
    const float page_t = ease_out_cubic(seconds_since(page_switch_time) / 0.2f);

    // Esc closes the overlay, unless the user is typing
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !io.WantTextInput) {
        ov.ShowOverlay(false);
        return;
    }

    ImGui::PushFont(ov.font_default, ov.settings->overlay_appearance.font_size * scale);
    auto style_counts = push_imgui_style(theme, scale);

    // backdrop + panel shadow live behind every window
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), to_u32(theme.backdrop, open_t));
    ui::draw_paw_pattern(bg, ImVec2(0, 0), ImVec2(W, H), to_u32(theme.accent, 0.05f * open_t), S(110.0f));

    const float win_w = std::min(W - S(48.0f), S(1200.0f));
    const float win_h = std::min(H - S(48.0f), S(760.0f));
    // the user can drag the window around, keep enough of it on screen to grab it again
    const float keep = S(120.0f);
    win_offset.x = std::clamp(win_offset.x, (keep - win_w - (W - win_w) * 0.5f) / W, (W - keep - (W - win_w) * 0.5f) / W);
    win_offset.y = std::clamp(win_offset.y, -((H - win_h) * 0.5f) / H, (H - keep - (H - win_h) * 0.5f) / H);
    const ImVec2 win_pos((W - win_w) * 0.5f + win_offset.x * W, (H - win_h) * 0.5f + win_offset.y * H + (1.0f - open_t) * S(40.0f));
    const float rounding = S(22.0f);
    ui::draw_shadow(bg, win_pos, ImVec2(win_pos.x + win_w, win_pos.y + win_h), rounding, S(28.0f), (theme.dark ? 0.55f : 0.25f) * open_t);
    bg->AddRectFilled(win_pos, ImVec2(win_pos.x + win_w, win_pos.y + win_h), to_u32(theme.panel, open_t), rounding);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, open_t);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::SetNextWindowPos(win_pos);
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h));
    if (ImGui::Begin("##pupberg_main", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        const float side_w = S(240.0f);
        render_sidebar(side_w, win_h);

        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28.0f), S(24.0f)));
        ImGui::BeginChild("##pupberg_content", ImVec2(win_w - side_w, win_h), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();

        // page header with a close button on the right
        static const char *titles[] = { "Home", "Friends", "Chat", "Network", "Gallery", "Settings" };
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, open_t * page_t);
        ImVec2 header_pos = ImGui::GetCursorScreenPos();
        ui::heading(titles[(int)page], 1.7f);
        float close_sz = S(36.0f);
        ImGui::SetCursorScreenPos(ImVec2(header_pos.x + ImGui::GetContentRegionAvail().x - close_sz, header_pos.y));
        if (ui::icon_button("##pupberg_close", Icon::Close, close_sz, "Close (Esc)")) {
            ov.ShowOverlay(false);
        }
        ImGui::SetCursorScreenPos(ImVec2(header_pos.x, header_pos.y + std::max(close_sz, ImGui::GetFontSize() * 1.7f) + S(10.0f)));

        // keyboard scrolling, there's no mouse wheel when the input fallback is used
        if (!ImGui::GetIO().WantTextInput) {
            float step = ImGui::GetWindowHeight() * 0.8f;
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) ImGui::SetScrollY(ImGui::GetScrollY() + step);
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) ImGui::SetScrollY(ImGui::GetScrollY() - step);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ImGui::SetScrollY(ImGui::GetScrollY() + S(60.0f));
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) ImGui::SetScrollY(ImGui::GetScrollY() - S(60.0f));
        }

        // slide the page content a bit when switching tabs
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1.0f - page_t) * S(14.0f));
        render_banners();
        switch (page) {
        case Page::Home: render_home(); break;
        case Page::Friends: render_friends(); break;
        case Page::Chat: render_chat(); break;
        case Page::Network: render_network(); break;
        case Page::Gallery: render_gallery(); break;
        case Page::Settings: render_settings(); break;
        }
        ui::spacer(16.0f);
        ImGui::PopStyleVar();

        ImGui::EndChild();

        // drag the window by any empty spot, double click one to center it again
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive()) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                win_offset = ImVec2(0, 0);
                dragging_window = false;
                save_prefs();
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                dragging_window = true;
            }
        }
        if (dragging_window) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                win_offset.x += io.MouseDelta.x / W;
                win_offset.y += io.MouseDelta.y / H;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            } else {
                dragging_window = false;
                save_prefs();
            }
        }
    } else {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
    ImGui::End();
    ImGui::PopStyleVar(); // alpha

    render_side_windows();

    pop_imgui_style(style_counts);
    ImGui::PopFont();
}

void PupOverlay::render_sidebar(float width, float height)
{
    ImGui::BeginChild("##pupberg_sidebar", ImVec2(width, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetWindowPos();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), to_u32(theme.sidebar), S(22.0f), ImDrawFlags_RoundCornersLeft);
    dl->AddLine(ImVec2(p.x + width - 1, p.y + S(20.0f)), ImVec2(p.x + width - 1, p.y + height - S(20.0f)), to_u32(theme.border, 0.6f));

    const float pad = S(16.0f);
    // mascot + brand
    float dog_r = S(28.0f);
    ImVec2 dog_c(p.x + pad + dog_r * 1.3f, p.y + S(28.0f) + dog_r);
    ui::draw_dog(dl, dog_c, dog_r, theme, ui::ctx().time * 3.0f);
    ImGui::SetCursorScreenPos(ImVec2(dog_c.x + dog_r * 1.3f + S(10.0f), dog_c.y - ImGui::GetFontSize() * 1.1f));
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.accent);
    ui::heading("PupBerg", 1.45f);
    ImGui::PopStyleColor();
    ui::text_muted("%s", ov.settings->get_local_name());
    ImGui::EndGroup();

    ImGui::SetCursorScreenPos(ImVec2(p.x + pad, dog_c.y + dog_r + S(28.0f)));
    struct NavEntry { const char *label; Icon icon; Page page; };
    static const NavEntry entries[] = {
        { "Home##nav", Icon::Home, Page::Home },
        { "Friends##nav", Icon::Friends, Page::Friends },
        { "Chat##nav", Icon::Chat, Page::Chat },
        { "Network##nav", Icon::Network, Page::Network },
        { "Gallery##nav", Icon::Camera, Page::Gallery },
        { "Settings##nav", Icon::Gear, Page::Settings },
    };
    for (const auto &e : entries) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + pad, ImGui::GetCursorScreenPos().y));
        if (ui::nav_item(e.label, e.icon, page == e.page, width - pad * 2.0f)) set_page(e.page);
        ImGui::SetCursorScreenPos(ImVec2(p.x + pad, ImGui::GetCursorScreenPos().y + S(2.0f)));
    }

    // footer: hotkey hint + version
    float fy = p.y + height - S(20.0f) - ImGui::GetTextLineHeightWithSpacing() * 3.0f;
    ImGui::SetCursorScreenPos(ImVec2(p.x + pad, fy));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + width - pad * 2.0f);
    ui::text_muted(ov.settings->pupberg.home_key_toggle ? "%s or Home to toggle" : "%s to toggle", key_combo_text().c_str());
    ui::text_muted("v%s - gbe_fork based", PUPBERG_VERSION);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::EndChild();
}

void PupOverlay::render_banners()
{
    if (ov.warn_bad_appid || ov.warn_local_save) {
        ui::begin_card("##warn_card");
        ImGui::PushStyleColor(ImGuiCol_Text, theme.warning);
        ImGui::TextUnformatted("Heads up!");
        ImGui::PopStyleColor();
        if (ov.warn_bad_appid) ui::text_muted("The app ID is 0 or missing. Put the game's app ID in steam_settings/steam_appid.txt or some features won't work.");
        if (ov.warn_local_save) ui::text_muted("Saves are stored in a local folder next to the game (local_save), make sure you back them up.");
        if (ui::button("Got it##warn_ok", ImVec2(0, 0), ButtonKind::Soft, Icon::Check)) {
            ov.warn_bad_appid = false;
            ov.warn_local_save = false;
        }
        ui::end_card();
        ui::spacer(10.0f);
    }

    if (ov.show_url.size()) {
        ui::begin_card("##url_card");
        ImGui::TextUnformatted("The game wants to open a web page");
        ImGui::PushItemWidth(-S(200.0f));
        std::string url = ov.show_url;
        ImGui::InputText("##pup_url", url.data(), url.size() + 1, ImGuiInputTextFlags_ReadOnly);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ui::button("Copy##url_copy", ImVec2(0, 0), ButtonKind::Soft, Icon::Copy)) ImGui::SetClipboardText(ov.show_url.c_str());
        ImGui::SameLine();
        if (ui::button("Close##url_close", ImVec2(0, 0), ButtonKind::Ghost)) ov.show_url.clear();
        ui::end_card();
        ui::spacer(10.0f);
    }
}

// ---------------------------------------------------------------------------
// Home

void PupOverlay::render_home()
{
    const float gap = S(14.0f);
    auto zt_state = zt->snapshot();
    std::string zt_ip{};
    for (const auto &n : zt_state.networks) {
        for (const auto &a : n.addresses) {
            uint32_t ip; int prefix;
            if (n.status == "OK" && ZeroTierClient::parse_ipv4_cidr(a, ip, prefix)) { zt_ip = ZeroTierClient::ipv4_to_string(ip); break; }
        }
        if (!zt_ip.empty()) break;
    }

    // hero
    ui::begin_card("##hero");
    {
        float dog_r = S(42.0f);
        ImVec2 cur = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(dog_r * 2.0f + S(10.0f), dog_r * 2.0f));
        ui::draw_dog(ImGui::GetWindowDrawList(), ImVec2(cur.x + dog_r + S(4.0f), cur.y + dog_r), dog_r, theme, ui::ctx().time * 4.0f);
        ImGui::SameLine();
        ImGui::BeginGroup();
        std::string hello = std::string("Hey ") + ov.settings->get_local_name() + "!";
        ui::heading(hello.c_str(), 1.5f);
        ui::text_muted("App ID %u  -  Steam ID %llu", ov.settings->get_local_game_id().AppID(),
                       (unsigned long long)ov.settings->get_local_steam_id().ConvertToUint64());
        if (ov.settings->record_playtime && ov.playtime_counter) {
            uint64_t sess = ov.playtime_counter->session_seconds();
            uint64_t total = ov.playtime_counter->seconds();
            ui::text_muted("Session %02u:%02u:%02u  -  Total %uh %02um",
                (unsigned)(sess / 3600), (unsigned)((sess / 60) % 60), (unsigned)(sess % 60),
                (unsigned)(total / 3600), (unsigned)((total % 3600) / 60));
        }
        ui::spacer(2.0f);
        ui::chip(ov.i_have_lobby ? "In a lobby" : "No lobby", ov.i_have_lobby ? theme.success : theme.text_muted);
        ImGui::SameLine();
        if (server_mode) {
            auto rs = ov.network->relay_status();
            bool connected = rs.state == Relay_Status::State::Connected;
            ui::chip(connected ? ("Room " + rs.room).c_str() : (rs.state == Relay_Status::State::Connecting ? "Joining room..." : "Online: no room"),
                     connected ? theme.success : theme.text_muted);
        } else {
            ui::chip(zt_ip.empty() ? "ZeroTier off" : ("ZeroTier " + zt_ip).c_str(), zt_ip.empty() ? theme.text_muted : theme.success);
        }
        ImGui::EndGroup();
    }
    ui::end_card();
    ui::spacer(14.0f);

    // stat tiles
    const float tile_w = (ImGui::GetContentRegionAvail().x - gap) / 2.0f;
    auto tile_header = [&](Icon icon, const char *label) {
        ImVec2 c = ImGui::GetCursorScreenPos();
        float sz = ImGui::GetFontSize() * 1.9f;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(c, ImVec2(c.x + sz, c.y + sz), to_u32(theme.accent, 0.16f), S(10.0f));
        ui::draw_icon(dl, icon, ImVec2(c.x + sz * 0.5f, c.y + sz * 0.5f), sz * 0.55f, to_u32(theme.accent));
        dl->AddText(ImVec2(c.x + sz + S(10.0f), c.y + (sz - ImGui::GetFontSize()) * 0.5f), to_u32(theme.text_muted), label);
        ImGui::Dummy(ImVec2(sz, sz));
    };

    ui::begin_card("##tile_friends", tile_w, true);
    tile_header(Icon::Friends, "Friends online");
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%zu", ov.friends.size());
        ui::heading(buf, 1.4f);
        ui::text_muted(ov.friends.empty() ? "Nobody yet, invite your pack!" : "Ready to play");
    }
    if (ui::end_card()) set_page(Page::Friends);
    ImGui::SameLine(0.0f, gap);

    ui::begin_card("##tile_net", tile_w, true);
    if (server_mode) {
        auto rs = ov.network->relay_status();
        tile_header(Icon::Network, "Online room");
        bool connected = rs.state == Relay_Status::State::Connected;
        ui::heading(connected ? rs.room.c_str() : "No room", 1.4f);
        if (connected) ui::text_muted("%zu %s with you", rs.members.size(), rs.members.size() == 1 ? "friend" : "friends");
        else ui::text_muted("%s", rs.state == Relay_Status::State::Error ? rs.error.c_str() : "Create or join a room");
    } else {
        tile_header(Icon::Network, "ZeroTier");
        ui::heading(zt_ip.empty() ? "Offline" : zt_ip.c_str(), 1.4f);
        ui::text_muted(!zt_state.token_found ? "Needs the auth token" :
                       !zt_state.service_online ? "Service not running" : (zt_ip.empty() ? "Join a network" : "Connected"));
    }
    if (ui::end_card()) set_page(Page::Network);
    ui::spacer(14.0f);

    // quick actions
    ui::begin_card("##actions");
    ImGui::TextUnformatted("Quick actions");
    ui::spacer(4.0f);
    if (ov.i_have_lobby && !ov.friends.empty()) {
        if (ui::button("Invite all friends##qa_inv", ImVec2(0, 0), ButtonKind::Primary, Icon::Friends)) ov.invite_all_friends_clicked = true;
        ImGui::SameLine();
    }
    if (ov.settings->overlay_show_button_screenshots) {
        if (ui::button("Gallery##qa_gal", ImVec2(0, 0), ButtonKind::Soft, Icon::Camera)) ov.show_screenshots_window = !ov.show_screenshots_window;
        ImGui::SameLine();
    }
    if (ov.settings->overlay_show_button_copy_id) {
        if (ui::button("Copy my ID##qa_id", ImVec2(0, 0), ButtonKind::Soft, Icon::Copy)) {
            ImGui::SetClipboardText(std::to_string(ov.settings->get_local_steam_id().ConvertToUint64()).c_str());
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ui::spacer(4.0f);
    if (ui::toggle("FPS##qa_fps", &ov.stats.show_fps)) ov.allow_renderer_frame_processing(ov.stats.show_fps);
    ImGui::SameLine(0.0f, S(24.0f));
    if (ui::toggle("Frametime##qa_ft", &ov.stats.show_frametime)) ov.allow_renderer_frame_processing(ov.stats.show_frametime);
    ImGui::SameLine(0.0f, S(24.0f));
    if (ui::toggle("Playtime##qa_pt", &ov.stats.show_playtime)) ov.allow_renderer_frame_processing(ov.stats.show_playtime);
    ui::end_card();
    ui::spacer(14.0f);

    // recent activity
    ui::begin_card("##activity");
    ImGui::TextUnformatted("Recent activity");
    ui::spacer(4.0f);
    if (ov.notification_history.empty()) {
        ui::text_muted("All quiet... good pup.");
    } else {
        int shown = 0;
        for (auto it = ov.notification_history.rbegin(); it != ov.notification_history.rend() && shown < 6; ++it, ++shown) {
            Icon icon = Icon::Bell;
            switch ((notification_type)it->type) {
            case notification_type::message: icon = Icon::Chat; break;
            case notification_type::invite:
            case notification_type::auto_accept_invite: icon = Icon::Friends; break;
            case notification_type::achievement:
            case notification_type::achievement_progress: icon = Icon::Trophy; break;
            case notification_type::screenshot: icon = Icon::Camera; break;
            }
            time_t secs = (time_t)std::chrono::duration_cast<std::chrono::seconds>(it->timestamp).count();
            struct tm tm_buf{};
#ifdef _MSC_VER
            localtime_s(&tm_buf, &secs);
#else
            localtime_r(&secs, &tm_buf);
#endif
            std::string msg = it->message;
            std::replace(msg.begin(), msg.end(), '\n', ' ');
            ImVec2 c = ImGui::GetCursorScreenPos();
            float fs = ImGui::GetFontSize();
            ImGui::Dummy(ImVec2(fs * 1.3f, fs));
            ui::draw_icon(ImGui::GetWindowDrawList(), icon, ImVec2(c.x + fs * 0.6f, c.y + fs * 0.5f), fs * 0.9f, to_u32(theme.accent));
            ImGui::SameLine();
            ImGui::TextColored(theme.text_muted, "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
            ImGui::SameLine();
            ImGui::TextWrapped("%s", msg.c_str());
        }
    }
    ui::end_card();
}

// ---------------------------------------------------------------------------
// Friends

void PupOverlay::render_friends()
{
    ImGui::PushItemWidth(std::min(S(360.0f), ImGui::GetContentRegionAvail().x * 0.5f));
    ImGui::InputTextWithHint("##friend_search", "Search friends...", friend_search, sizeof(friend_search));
    ImGui::PopItemWidth();
    if (ov.i_have_lobby && !ov.friends.empty()) {
        ImGui::SameLine();
        if (ui::button("Invite all##fr_inv_all", ImVec2(0, 0), ButtonKind::Primary, Icon::Friends)) ov.invite_all_friends_clicked = true;
    }
    ui::spacer(10.0f);

    if (ov.friends.empty()) {
        ui::begin_card("##no_friends");
        float r = S(50.0f);
        ImVec2 c = ImGui::GetCursorScreenPos();
        float w = ui::card_inner_right() - c.x;
        ImGui::Dummy(ImVec2(w, r * 2.0f + S(8.0f)));
        ui::draw_dog(ImGui::GetWindowDrawList(), ImVec2(c.x + w * 0.5f, c.y + r), r, theme, ui::ctx().time * 2.0f);
        const char *t1 = "No pups online yet";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - ImGui::CalcTextSize(t1).x * 1.2f) * 0.5f);
        ui::heading(t1, 1.2f);
        ui::text_muted("Friends running PupBerg on the same LAN show up here automatically. Playing over the internet? "
                       "Join the same online room (Public Server) or ZeroTier network in the Network tab and they'll pop up in a few seconds.");
        ui::spacer(6.0f);
        if (ui::button("Open Network##fr_go_net", ImVec2(0, 0), ButtonKind::Primary, Icon::Network)) set_page(Page::Network);
        ui::end_card();
        return;
    }

    int idx = 0;
    for (auto &entry : ov.friends) {
        const Friend &frd = entry.first;
        friend_window_state &state = entry.second;
        if (!contains_insensitive(frd.name(), friend_search)) continue;

        ImGui::PushID((int)frd.id());
        ui::begin_card("##friend_card");
        float row_h = S(48.0f);
        ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(row_h, row_h));
        ui::draw_avatar(ImGui::GetWindowDrawList(), ImVec2(row.x + row_h * 0.5f, row.y + row_h * 0.5f), row_h * 0.5f, frd.name(), true);
        ImGui::SameLine(0.0f, S(14.0f));
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4.0f));
        ImGui::TextUnformatted(frd.name().c_str());
        bool same_app = ov.settings->get_local_game_id().AppID() == frd.appid();
        if (state.window_state & (window_state_lobby_invite | window_state_rich_invite)) {
            ImGui::TextColored(theme.accent, "Invited you to play!");
        } else if (same_app) {
            ui::text_muted(state.joinable ? "Playing this game - joinable" : "Playing this game");
        } else {
            ui::text_muted("In app %u", frd.appid());
        }
        ImGui::EndGroup();

        // right aligned actions
        const float bw = S(96.0f), bh = ImGui::GetFontSize() + S(14.0f), sp = S(8.0f);
        bool can_invite = same_app && ov.i_have_lobby;
        bool can_join = same_app && state.joinable;
        int count = 2 + (can_invite ? 1 : 0) + (can_join ? 1 : 0);
        float x = ui::card_inner_right() - count * bw - (count - 1) * sp;
        float y = row.y + (row_h - bh) * 0.5f;
        auto place = [&]() { ImGui::SetCursorScreenPos(ImVec2(x, y)); x += bw + sp; };

        place();
        if (ui::button("Chat##fr_chat", ImVec2(bw, bh), ButtonKind::Soft, Icon::Chat)) {
            open_chat(frd.id());
        }
        if (can_invite) {
            place();
            if (ui::button("Invite##fr_inv", ImVec2(bw, bh), ButtonKind::Primary, Icon::Plus)) {
                state.window_state |= window_state_invite;
                ov.has_friend_action.push(frd);
            }
        }
        if (can_join) {
            place();
            if (ui::button("Join##fr_join", ImVec2(bw, bh), ButtonKind::Primary, Icon::Paw) && !ov.invite_all_friends_clicked) {
                state.window_state |= window_state_join;
                ov.has_friend_action.push(frd);
            }
        }
        place();
        if (ui::button("ID##fr_id", ImVec2(bw, bh), ButtonKind::Ghost, Icon::Copy)) {
            ImGui::SetClipboardText(std::to_string(frd.id()).c_str());
        }
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + row_h));
        ImGui::Dummy(ImVec2(1, 0));
        ui::end_card();
        ImGui::PopID();
        ui::spacer(8.0f);
        ++idx;
    }
    if (!idx) ui::text_muted("No friend matches \"%s\"", friend_search);
}

// ---------------------------------------------------------------------------
// Network (ZeroTier)

static void status_chip(const std::string &status, const Theme &t)
{
    if (status == "OK") ui::chip("Connected", t.success);
    else if (status == "REQUESTING_CONFIGURATION") ui::chip("Waiting for authorization", t.warning);
    else if (status == "ACCESS_DENIED") ui::chip("Not authorized", t.danger);
    else if (status == "NOT_FOUND") ui::chip("Network not found", t.danger);
    else ui::chip(status.empty() ? "Unknown" : status.c_str(), t.text_muted);
}

void PupOverlay::render_network()
{
    // how friends are found over the internet, LAN always works on top of either
    static const char *modes[] = { "ZeroTier", "Public Server" };
    int mode = server_mode ? 1 : 0;
    if (ui::segmented("##net_mode", modes, 2, &mode)) set_network_mode(mode == 1);
    ui::text_muted(server_mode ? "Play over the internet with a room code through the PupBerg lobby server, no VPN needed."
                               : "Play over a ZeroTier network, everyone installs ZeroTier One.");
    ui::spacer(12.0f);

    if (server_mode) render_lobby_server();
    else render_zerotier();
}

void PupOverlay::set_network_mode(bool server)
{
    if (server == server_mode) return;
    server_mode = server;
    if (!server_mode) ov.network->relay_leave();
    else lobby_rooms_requested = false; // refresh the public room list
    save_prefs();
}

bool PupOverlay::valid_room_code(const std::string &code)
{
    if (code.size() < 4 || code.size() > 32) return false;
    return std::all_of(code.begin(), code.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}

std::string PupOverlay::random_room_code()
{
    // no 0/O/1/I so codes are easy to read out loud
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, (int)sizeof(alphabet) - 2);
    std::string code = "PUP-";
    for (int i = 0; i < 5; ++i) code.push_back(alphabet[dist(rd)]);
    return code;
}

void PupOverlay::join_room(const std::string &code)
{
    std::string room(code);
    std::transform(room.begin(), room.end(), room.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    std::memset(lobby_room_input, 0, sizeof(lobby_room_input));
    std::strncpy(lobby_room_input, room.c_str(), sizeof(lobby_room_input) - 1);
    ov.network->relay_join(lobby_server_input, room, ov.settings->get_local_name(), lobby_public);
}

void PupOverlay::render_lobby_server()
{
    auto st = ov.network->relay_status();
    const uint32 appid = ov.settings->get_local_game_id().AppID();
    using State = Relay_Status::State;
    bool in_room = st.state == State::Connected || st.state == State::Connecting;

    // keep the public room list fresh while it's on screen, rooms made after opening the page show up by themselves
    if (!lobby_rooms_requested || (!st.rooms_loading && seconds_since(lobby_rooms_time) > 5.0f)) {
        ov.network->relay_request_rooms(lobby_server_input, appid);
        lobby_rooms_requested = true;
        lobby_rooms_time = std::chrono::steady_clock::now();
    }

    // status + current room
    ui::begin_card("##lobby_status");
    {
        ImGui::TextUnformatted("Lobby server");
        ImGui::SameLine(0.0f, S(12.0f));
        switch (st.state) {
        case State::Off: ui::chip("Not in a room", theme.text_muted); break;
        case State::Connecting: ui::chip("Connecting...", theme.warning); break;
        case State::Connected: ui::chip("Connected", theme.success); break;
        case State::Error: ui::chip("Error", theme.danger); break;
        }
        if (in_room) {
            ImGui::SameLine();
            ui::chip(st.is_public ? "Public" : "Private", st.is_public ? theme.accent : theme.accent2);
        }
        if (!st.error.empty()) ImGui::TextColored(theme.danger, "%s", st.error.c_str());

        if (in_room) {
            ui::spacer(6.0f);
            ui::text_muted("Room code");
            ui::heading(st.room.c_str(), 1.5f);
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(6.0f));
            if (ui::button("Copy code##lobby_copy", ImVec2(0, 0), ButtonKind::Soft, Icon::Copy)) ImGui::SetClipboardText(st.room.c_str());
            ImGui::SameLine();
            if (ui::button("Leave##lobby_leave", ImVec2(0, 0), ButtonKind::Danger)) {
                ov.network->relay_leave();
                lobby_rooms_requested = false;
            }

            ui::spacer(6.0f);
            if (st.members.empty()) {
                ui::text_muted(st.state == State::Connected ? "Nobody else here yet, send your friends the room code!" : "Joining the room...");
            }
            for (const auto &m : st.members) {
                ImGui::PushID((int)(m.id & 0x7FFFFFFF));
                ImVec2 c = ImGui::GetCursorScreenPos();
                float r = ImGui::GetFontSize() * 0.75f;
                ui::draw_avatar(ImGui::GetWindowDrawList(), ImVec2(c.x + r, c.y + r), r, m.name, true);
                ImGui::Dummy(ImVec2(r * 2.0f, r * 2.0f));
                ImGui::SameLine();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (r * 2.0f - ImGui::GetFontSize()) * 0.5f);
                ImGui::TextUnformatted(m.name.empty() ? "(no name)" : m.name.c_str());
                if (m.appid != appid) {
                    ImGui::SameLine();
                    ImGui::TextColored(theme.text_muted, "playing another game (%u)", m.appid);
                }
                ImGui::PopID();
            }
        }
    }
    ui::end_card();
    ui::spacer(12.0f);

    // create / join
    ui::begin_card("##lobby_join");
    {
        ImGui::TextUnformatted("Create a room");
        ui::text_muted("Private rooms can only be joined with the code. Public rooms also show up below for everyone playing this game.");
        ui::spacer(4.0f);
        static const char *vis[] = { "Private", "Public" };
        int v = lobby_public ? 1 : 0;
        if (ui::segmented("##lobby_vis", vis, 2, &v)) {
            lobby_public = v == 1;
            save_prefs();
        }
        ImGui::SameLine(0.0f, S(16.0f));
        if (ui::button("Create room##lobby_create", ImVec2(0, 0), ButtonKind::Primary, Icon::Plus)) {
            join_room(random_room_code());
            save_prefs();
        }

        ui::spacer(10.0f);
        ImGui::TextUnformatted("Join with a code");
        ImGui::PushItemWidth(S(220.0f));
        bool enter = ImGui::InputTextWithHint("##lobby_code", "PUP-XXXXX", lobby_room_input, sizeof(lobby_room_input),
                                              ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        bool valid = valid_room_code(lobby_room_input);
        if (ui::button("Join##lobby_join_btn", ImVec2(0, 0), ButtonKind::Primary, Icon::Paw, valid) || (enter && valid)) {
            join_room(lobby_room_input);
            save_prefs();
        }
        ImGui::SameLine(0.0f, S(20.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4.0f));
        if (ui::toggle("Rejoin when the game starts##lobby_auto", &lobby_auto_join)) save_prefs();
    }
    ui::end_card();
    ui::spacer(12.0f);

    // public rooms for this game
    ui::begin_card("##lobby_rooms");
    {
        ImGui::TextUnformatted("Public rooms");
        ImGui::SameLine();
        float rx = ui::card_inner_right() - S(110.0f);
        ImGui::SetCursorScreenPos(ImVec2(rx, ImGui::GetCursorScreenPos().y));
        if (ui::button(st.rooms_loading ? "Loading...##lobby_ref" : "Refresh##lobby_ref", ImVec2(S(110.0f), 0), ButtonKind::Ghost, Icon::None, !st.rooms_loading)) {
            ov.network->relay_request_rooms(lobby_server_input, appid);
        }
        size_t shown = 0;
        for (const auto &r : st.rooms) {
            if (r.code == st.room && in_room) continue;
            ImGui::PushID(r.code.c_str());
            ui::spacer(4.0f);
            ImVec2 row = ImGui::GetCursorScreenPos();
            ImGui::BeginGroup();
            ImGui::TextUnformatted(r.code.c_str());
            ImGui::TextColored(theme.text_muted, "hosted by %s  -  %u %s", r.host.c_str(), r.members, r.members == 1 ? "player" : "players");
            ImGui::EndGroup();
            float bottom = ImGui::GetItemRectMax().y;
            float bw = S(90.0f);
            ImGui::SetCursorScreenPos(ImVec2(ui::card_inner_right() - bw, row.y));
            if (ui::button("Join##room_join", ImVec2(bw, 0), ButtonKind::Primary)) {
                join_room(r.code);
                save_prefs();
            }
            ImGui::SetCursorScreenPos(ImVec2(row.x, std::max(bottom, ImGui::GetItemRectMax().y) + S(4.0f)));
            // ImGui asserts if a card ends right after a SetCursorScreenPos() that grows it, submit an item
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
            ++shown;
        }
        if (!shown) ui::text_muted(st.rooms_loading ? "Looking for rooms..." : "No public rooms for this game right now. Create one!");
    }
    ui::end_card();
    ui::spacer(12.0f);

    // server address
    ui::begin_card("##lobby_server");
    {
        ImGui::TextUnformatted("Server");
        ui::text_muted("Everyone has to use the same lobby server. The default one is run by PupBerg.");
        ImGui::PushItemWidth(S(300.0f));
        ImGui::InputTextWithHint("##lobby_srv", "host:port", lobby_server_input, sizeof(lobby_server_input));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save_prefs();
            lobby_rooms_requested = false;
        }
        ImGui::PopItemWidth();
    }
    ui::end_card();
}

void PupOverlay::render_zerotier()
{
    auto st = zt->snapshot();

    // status
    ui::begin_card("##zt_status");
    {
        ImGui::TextUnformatted("ZeroTier");
        ImGui::SameLine(0.0f, S(12.0f));
        if (!st.token_found) ui::chip("No auth token", theme.warning);
        else ui::chip(st.service_online ? "Service running" : "Service offline", st.service_online ? theme.success : theme.danger);
        if (st.service_online) {
            ImGui::SameLine();
            ui::chip(st.node_online ? "Online" : "Connecting...", st.node_online ? theme.success : theme.warning);
        }
        ImGui::SameLine();
        float rx = ui::card_inner_right() - S(110.0f);
        ImGui::SetCursorScreenPos(ImVec2(rx, ImGui::GetCursorScreenPos().y));
        if (ui::button(st.busy ? "Working...##zt_ref" : "Refresh##zt_ref", ImVec2(S(110.0f), 0), ButtonKind::Ghost, Icon::None, !st.busy)) zt->refresh();

        if (!st.node_id.empty()) ui::text_muted("Node %s  -  version %s", st.node_id.c_str(), st.version.c_str());
        if (!st.last_error.empty()) ImGui::TextColored(theme.danger, "%s", st.last_error.c_str());

        if (!st.token_found) {
            ui::text_muted("PupBerg needs the ZeroTier auth token to control the service. On Linux run this once in a terminal:");
            static const char cmd[] = "sudo cat /var/lib/zerotier-one/authtoken.secret > ~/.zeroTierOneAuthToken";
            ImGui::PushStyleColor(ImGuiCol_Text, theme.accent);
            ImGui::TextUnformatted(cmd);
            ImGui::PopStyleColor();
            if (ui::button("Copy command##zt_cmd", ImVec2(0, 0), ButtonKind::Soft, Icon::Copy)) ImGui::SetClipboardText(cmd);
            ui::text_muted("On Windows, run the game as admin once or set zerotier_token in configs.overlay.ini.");
        } else if (!st.service_online) {
            ui::text_muted("Install ZeroTier One from zerotier.com and make sure the service is running "
                           "(Linux: sudo systemctl enable --now zerotier-one).");
        }
    }
    ui::end_card();
    ui::spacer(12.0f);

    // join
    ui::begin_card("##zt_join");
    {
        ImGui::TextUnformatted("Join a network");
        ui::text_muted("Create a free network at my.zerotier.com, share its ID with your friends, then authorize them in the network's Members list.");
        ui::spacer(4.0f);
        ImGui::PushItemWidth(S(260.0f));
        bool enter = ImGui::InputTextWithHint("##zt_net", "Network ID (16 hex chars)", zt_network_input, sizeof(zt_network_input),
                                              ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemDeactivatedAfterEdit()) save_prefs();
        ImGui::PopItemWidth();
        ImGui::SameLine();
        bool valid = ZeroTierClient::valid_network_id(zt_network_input);
        if ((ui::button("Join##zt_join_btn", ImVec2(0, 0), ButtonKind::Primary, Icon::Paw, valid && st.service_online) || (enter && valid))) {
            std::string id(zt_network_input);
            std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            std::strncpy(zt_network_input, id.c_str(), sizeof(zt_network_input) - 1);
            zt->join(id);
            save_prefs();
        }
        ImGui::SameLine(0.0f, S(20.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4.0f));
        if (ui::toggle("Auto-join when the game starts##zt_auto", &zt_auto_join)) save_prefs();
    }
    ui::end_card();
    ui::spacer(12.0f);

    // joined networks
    ui::begin_card("##zt_nets");
    ImGui::TextUnformatted("Your networks");
    if (st.networks.empty()) {
        ui::text_muted("You haven't joined any network yet.");
    }
    for (const auto &n : st.networks) {
        ImGui::PushID(n.id.c_str());
        ui::spacer(6.0f);
        ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::BeginGroup();
        ImGui::Text("%s", n.name.empty() ? "(unnamed network)" : n.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(theme.text_muted, "%s", n.id.c_str());
        status_chip(n.status, theme);
        for (const auto &a : n.addresses) {
            uint32_t ip; int prefix;
            bool v4 = ZeroTierClient::parse_ipv4_cidr(a, ip, prefix);
            std::string shown = v4 ? ZeroTierClient::ipv4_to_string(ip) : a;
            ImGui::TextUnformatted(shown.c_str());
            ImGui::SameLine();
            ImGui::PushID(a.c_str());
            if (ui::icon_button("##copy_ip", Icon::Copy, ImGui::GetFontSize() * 1.3f, "Copy IP")) ImGui::SetClipboardText(shown.c_str());
            ImGui::PopID();
        }
        ImGui::EndGroup();
        float group_bottom = ImGui::GetItemRectMax().y;
        float bw = S(100.0f);
        ImGui::SetCursorScreenPos(ImVec2(ui::card_inner_right() - bw, row.y));
        if (ui::button("Leave##zt_leave", ImVec2(bw, 0), ButtonKind::Danger)) zt->leave(n.id);
        ImGui::SetCursorScreenPos(ImVec2(row.x, std::max(group_bottom, ImGui::GetItemRectMax().y) + S(6.0f)));
        ImGui::Separator();
        ImGui::PopID();
    }
    ui::end_card();
    ui::spacer(12.0f);

    // manual peers
    ui::begin_card("##zt_peers");
    {
        ImGui::TextUnformatted("Friend IPs");
        ui::text_muted("Friends on your ZeroTier network are found automatically. If someone doesn't show up, add their ZeroTier IP here and PupBerg will knock on their door directly.");
        ui::spacer(4.0f);
        ImGui::PushItemWidth(S(220.0f));
        bool enter = ImGui::InputTextWithHint("##peer_ip", "10.147.17.42", peer_ip_input, sizeof(peer_ip_input), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        uint32_t ip{};
        bool valid = ZeroTierClient::parse_ipv4(peer_ip_input, ip);
        if (ui::button("Add##peer_add", ImVec2(0, 0), ButtonKind::Primary, Icon::Plus, valid) || (enter && valid)) {
            std::string s = ZeroTierClient::ipv4_to_string(ip);
            if (std::find(peer_ips.begin(), peer_ips.end(), s) == peer_ips.end()) peer_ips.push_back(s);
            add_broadcast(ip);
            peer_ip_input[0] = 0;
            save_prefs();
        }
        for (size_t i = 0; i < peer_ips.size(); ++i) {
            ImGui::PushID((int)i);
            ImGui::TextUnformatted(peer_ips[i].c_str());
            ImGui::SameLine();
            if (ui::icon_button("##rm_peer", Icon::Close, ImGui::GetFontSize() * 1.3f, "Remove (takes effect after restart)")) {
                peer_ips.erase(peer_ips.begin() + i);
                save_prefs();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }
    ui::end_card();
    ui::spacer(12.0f);

    // how to
    ui::begin_card("##zt_howto");
    ImGui::TextUnformatted("Playing with friends, step by step");
    static const char *steps[] = {
        "Everyone installs ZeroTier One and uses PupBerg with the same game version.",
        "One person creates a network at my.zerotier.com and shares the Network ID.",
        "Everyone pastes the ID above and presses Join.",
        "The network owner ticks 'Auth' for each member on my.zerotier.com.",
        "Once everyone shows Connected, friends appear in the Friends tab. Host a lobby and invite them!",
    };
    for (int i = 0; i < 5; ++i) {
        ImVec2 c = ImGui::GetCursorScreenPos();
        float r = ImGui::GetFontSize() * 0.7f;
        ImGui::Dummy(ImVec2(r * 2.0f, r * 2.0f));
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(c.x + r, c.y + r), r, to_u32(theme.accent), 20);
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        ImVec2 ts = ImGui::CalcTextSize(num);
        dl->AddText(ImVec2(c.x + r - ts.x * 0.5f, c.y + r - ts.y * 0.5f), to_u32(theme.on_accent), num);
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (r * 2.0f - ImGui::GetFontSize()) * 0.5f);
        ImGui::TextWrapped("%s", steps[i]);
    }
    ui::end_card();
}

// ---------------------------------------------------------------------------
// Gallery

void PupOverlay::render_gallery()
{
    ui::begin_card("##gallery");
    {
        ImGui::TextUnformatted("Screenshots");
        if (ov.settings->enable_screenshot) {
            ui::text_muted("Take screenshots in game with your screenshot hotkey (F12 by default), they show up in the gallery.");
        } else {
            ui::text_muted("Screenshots are disabled, enable them in configs.overlay.ini.");
        }
        ui::spacer(4.0f);
        if (ui::button(ov.show_screenshots_window ? "Hide gallery##gal_toggle" : "Open gallery##gal_toggle", ImVec2(0, 0), ButtonKind::Primary, Icon::Camera)) {
            ov.show_screenshots_window = !ov.show_screenshots_window;
        }
        if (ov.settings->enable_screenshot && ov._renderer) {
            ImGui::SameLine();
            if (ui::button("Snap now##gal_snap", ImVec2(0, 0), ButtonKind::Soft, Icon::Paw)) {
                ov._renderer->TakeScreenshot(InGameOverlay::ScreenshotType_t::BeforeOverlay);
            }
        }
    }
    ui::end_card();
    ui::spacer(12.0f);

    ui::begin_card("##history");
    ImGui::TextUnformatted("Notification history");
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(ui::card_inner_right() - S(100.0f), ImGui::GetCursorScreenPos().y));
    if (ui::button("Clear##hist_clear", ImVec2(S(100.0f), 0), ButtonKind::Ghost)) {
        ov.notification_history.clear();
        ov.notification_history_cache.clear();
        ov.notification_history_cache_dirty = false;
    }
    if (ov.notification_history.empty()) ui::text_muted("Nothing yet.");
    for (auto it = ov.notification_history.rbegin(); it != ov.notification_history.rend(); ++it) {
        std::string msg = it->message;
        std::replace(msg.begin(), msg.end(), '\n', ' ');
        ImGui::BulletText("%s", msg.c_str());
    }
    ui::end_card();
}

// ---------------------------------------------------------------------------
// Settings

void PupOverlay::render_settings()
{
    // theme picker
    ui::begin_card("##themes");
    ImGui::TextUnformatted("Theme");
    ui::spacer(6.0f);
    {
        std::vector<const Theme *> list{};
        for (const auto &t : builtin_themes()) list.push_back(&t);
        list.push_back(&custom_theme);

        const float sw = S(150.0f), sh = S(96.0f), gap = S(12.0f);
        float start_x = ImGui::GetCursorScreenPos().x;
        float right = ui::card_inner_right();
        ImVec2 cur = ImGui::GetCursorScreenPos();
        for (const Theme *t : list) {
            if (cur.x + sw > right && cur.x > start_x) {
                cur.x = start_x;
                cur.y += sh + gap;
            }
            ImGui::SetCursorScreenPos(cur);
            ImGui::PushID(t->name.c_str());
            bool clicked = ImGui::InvisibleButton("##theme", ImVec2(sw, sh));
            bool hovered = ImGui::IsItemHovered();
            float ha = ui::anim(ImGui::GetItemID(), hovered);
            ImGui::PopID();
            if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            bool selected = theme_name == t->name;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImVec2 a(cur.x, cur.y - ha * S(2.0f)), b(cur.x + sw, cur.y + sh - ha * S(2.0f));
            dl->AddRectFilled(a, b, to_u32(with_alpha(t->panel, 1.0f)), S(12.0f));
            dl->AddRectFilled(a, ImVec2(a.x + S(36.0f), b.y), to_u32(t->sidebar), S(12.0f), ImDrawFlags_RoundCornersLeft);
            dl->AddRectFilled(ImVec2(a.x + S(46.0f), a.y + S(14.0f)), ImVec2(b.x - S(12.0f), a.y + S(40.0f)), to_u32(t->card), S(6.0f));
            dl->AddRectFilled(ImVec2(a.x + S(46.0f), a.y + S(48.0f)), ImVec2(a.x + S(96.0f), a.y + S(62.0f)), to_u32(t->accent), S(7.0f));
            ui::draw_icon(dl, Icon::Paw, ImVec2(a.x + S(18.0f), a.y + S(22.0f)), S(16.0f), to_u32(t->accent));
            ImVec2 ts = ImGui::CalcTextSize(t->name.c_str());
            dl->AddText(ImVec2(a.x + S(46.0f), b.y - ts.y - S(8.0f)), to_u32(t->text), t->name.c_str());
            dl->AddRect(a, b, selected ? to_u32(theme.accent) : to_u32(theme.border, 0.8f + ha * 0.2f), S(12.0f), 0, selected ? S(3.0f) : 1.0f);

            if (clicked) {
                select_theme(t->name);
                save_prefs();
            }
            cur.x += sw + gap;
        }
        ImGui::SetCursorScreenPos(ImVec2(start_x, cur.y + sh));
        ImGui::Dummy(ImVec2(1, S(4.0f)));
    }

    if (theme_name == CUSTOM_THEME_NAME) {
        ui::spacer(6.0f);
        ui::text_muted("Your own colors, saved automatically.");
        bool changed = false;
        const ImGuiColorEditFlags flags = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs;
        struct { const char *label; ImVec4 *c; } edits[] = {
            { "Accent", &custom_theme.accent }, { "Accent 2", &custom_theme.accent2 }, { "Text on accent", &custom_theme.on_accent },
            { "Window", &custom_theme.panel }, { "Sidebar", &custom_theme.sidebar }, { "Cards", &custom_theme.card },
            { "Cards hovered", &custom_theme.card_hover }, { "Borders", &custom_theme.border }, { "Text", &custom_theme.text },
            { "Muted text", &custom_theme.text_muted }, { "Backdrop", &custom_theme.backdrop },
        };
        int i = 0;
        for (auto &e : edits) {
            if (i % 4) ImGui::SameLine(0.0f, S(26.0f));
            changed |= ImGui::ColorEdit4(e.label, &e.c->x, flags);
            if (ImGui::IsItemDeactivatedAfterEdit()) save_prefs();
            ++i;
        }
        if (ui::toggle("Dark theme (stronger shadows)##custom_dark", &custom_theme.dark)) save_prefs();
        if (changed) theme = custom_theme;
    }
    ui::end_card();
    ui::spacer(12.0f);

    // size
    ui::begin_card("##scale");
    ImGui::TextUnformatted("Overlay size");
    ImGui::PushItemWidth(S(320.0f));
    ImGui::SliderFloat("##ui_scale", &ui_scale, 0.75f, 1.75f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) save_prefs();
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ui::button("Reset##scale_reset", ImVec2(0, 0), ButtonKind::Ghost)) { ui_scale = 1.0f; save_prefs(); }
    ui::end_card();
    ui::spacer(12.0f);

    // profile
    ui::begin_card("##profile");
    {
        ImGui::TextUnformatted("Profile");
        ui::spacer(4.0f);
        ImGui::PushItemWidth(S(320.0f));
        ImGui::InputTextWithHint("##username", "Your name", ov.username_text, sizeof(ov.username_text));
        int lang_count = 0;
        const char *const *langs = Steam_Overlay::get_valid_languages(lang_count);
        if (ov.current_language < 0 || ov.current_language >= lang_count) ov.current_language = 0;
        if (ImGui::BeginCombo("##language", langs[ov.current_language])) {
            for (int i = 0; i < lang_count; ++i) {
                if (ImGui::Selectable(langs[i], i == ov.current_language)) ov.current_language = i;
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        if (ui::button("Save##profile_save", ImVec2(0, 0), ButtonKind::Primary, Icon::Check)) ov.save_settings = true;
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(6.0f));
        ui::text_muted("Restart the game to fully apply.");
    }
    ui::end_card();
    ui::spacer(12.0f);

    // overlay
    ui::begin_card("##overlay_opts");
    {
        ImGui::TextUnformatted("Overlay");
        ui::text_muted("Toggle hotkey: %s (change it in configs.overlay.ini, [overlay::hotkeys])", key_combo_text().c_str());
        ui::text_muted("Drag the window by any empty spot to move it, double click one to center it again.");
        ui::spacer(4.0f);
        if (ui::button("Center window##center_window", ImVec2(0, 0), ButtonKind::Ghost)) {
            win_offset = ImVec2(0, 0);
            save_prefs();
        }
        ImGui::SameLine();
        if (ui::button("Switch to classic overlay##go_classic", ImVec2(0, 0), ButtonKind::Soft, Icon::None)) {
            ov.settings->pupberg.classic_frontend = true;
            save_prefs();
        }
    }
    ui::end_card();
    ui::spacer(12.0f);

    ui::begin_card("##about");
    ImGui::Text("PupBerg v%s", PUPBERG_VERSION);
    ui::text_muted("A gbe_fork (Goldberg Steam Emulator) fork with a puppy flavored overlay. "
                   "Thanks to Mr. Goldberg, the gbe_fork contributors and Nemirtingas' Ingame Overlay project.");
    ui::end_card();
}

// friend chat windows + the screenshots gallery are regular ImGui windows, drawn with the PupBerg style
// ---------------------------------------------------------------------------
// Chat

void PupOverlay::open_chat(uint64_t friend_id)
{
    chat_friend_id = friend_id;
    chat_seen_len = 0;
    chat_focus_input = true;
    set_page(Page::Chat);
}

void PupOverlay::render_chat()
{
    if (ov.friends.empty()) {
        ui::begin_card("##chat_empty");
        ui::heading("Nobody to chat with yet", 1.2f);
        ui::text_muted("Friends show up here once they're online in PupBerg (Friends and Network tabs).");
        ui::end_card();
        return;
    }

    std::pair<const Friend, friend_window_state> *sel = nullptr;
    for (auto &e : ov.friends) {
        if ((uint64)e.first.id() == chat_friend_id) sel = &e;
    }
    if (!sel) {
        sel = &*ov.friends.begin();
        chat_friend_id = sel->first.id();
        chat_seen_len = 0;
    }
    // reading it counts as seen
    sel->second.window_state &= ~window_state_need_attention;

    const float h = std::max(S(360.0f), ImGui::GetContentRegionAvail().y - S(8.0f));
    const float list_w = S(240.0f);

    // friend list
    ImGui::BeginChild("##chat_list", ImVec2(list_w, h), ImGuiChildFlags_None);
    for (auto &e : ov.friends) {
        const Friend &frd = e.first;
        bool selected = &e == sel;
        bool unread = (e.second.window_state & window_state_need_attention) != 0;
        ImGui::PushID((int)frd.id());
        const float row_h = S(48.0f);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##chat_sel", selected, ImGuiSelectableFlags_None, ImVec2(list_w - S(8.0f), row_h))) {
            chat_friend_id = frd.id();
            chat_seen_len = 0;
            chat_focus_input = true;
        }
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float r = row_h * 0.36f;
        ui::draw_avatar(dl, ImVec2(p.x + S(8.0f) + r, p.y + row_h * 0.5f), r, frd.name(), true);
        dl->AddText(ImVec2(p.x + S(16.0f) + r * 2.0f, p.y + (row_h - ImGui::GetFontSize()) * 0.5f),
                    to_u32(selected ? theme.accent : theme.text), frd.name().c_str());
        if (unread) {
            dl->AddCircleFilled(ImVec2(p.x + list_w - S(24.0f), p.y + row_h * 0.5f), S(5.0f), to_u32(theme.accent), 16);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine(0.0f, S(14.0f));

    // conversation
    friend_window_state &state = sel->second;
    const Friend &frd = sel->first;
    ImGui::BeginChild("##chat_conv", ImVec2(0, h), ImGuiChildFlags_None);
    {
        ui::heading(frd.name().c_str(), 1.25f);
        bool same_app = ov.settings->get_local_game_id().AppID() == frd.appid();
        ui::text_muted(same_app ? "Playing this game" : "Playing another game (%u)", frd.appid());
        ui::spacer(6.0f);

        const float input_h = ImGui::GetFrameHeight() + S(16.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.card);
        ImGui::BeginChild("##chat_history", ImVec2(0, ImGui::GetContentRegionAvail().y - input_h), ImGuiChildFlags_AlwaysUseWindowPadding);
        {
            const std::string me = std::string(ov.settings->get_local_name()) + ": ";
            const std::string &hist = state.chat_history;
            if (hist.empty()) ui::text_muted("No messages yet, say hi!");
            size_t pos = 0;
            while (pos < hist.size()) {
                size_t nl = hist.find('\n', pos);
                if (nl == std::string::npos) nl = hist.size();
                std::string line = hist.substr(pos, nl - pos);
                pos = nl + 1;
                if (line.empty()) continue;
                bool mine = line.rfind(me, 0) == 0;
                size_t colon = line.find(": ");
                if (colon != std::string::npos) {
                    ImGui::PushStyleColor(ImGuiCol_Text, mine ? theme.accent : theme.accent2);
                    ImGui::TextUnformatted(line.substr(0, colon).c_str());
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.0f, S(8.0f));
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(line.substr(colon + 2).c_str());
                    ImGui::PopTextWrapPos();
                } else {
                    ImGui::TextWrapped("%s", line.c_str());
                }
                ui::spacer(2.0f);
            }
            // follow new messages
            if (hist.size() != chat_seen_len) {
                ImGui::SetScrollHereY(1.0f);
                chat_seen_len = hist.size();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ui::spacer(8.0f);
        const float send_w = S(110.0f);
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - send_w - S(10.0f));
        if (chat_focus_input) {
            ImGui::SetKeyboardFocusHere();
            chat_focus_input = false;
        }
        bool send = ImGui::InputTextWithHint("##chat_input", "Message...", state.chat_input, max_chat_len, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        ImGui::SameLine(0.0f, S(10.0f));
        if (ui::button("Send##chat_send", ImVec2(send_w, 0), ButtonKind::Primary, Icon::Chat, state.chat_input[0] != 0)) send = true;
        if (send && state.chat_input[0]) {
            // same path as the classic chat window, sent on the next overlay callback
            if (!(state.window_state & window_state_send_message)) {
                state.window_state |= window_state_send_message;
                ov.has_friend_action.push(frd);
            }
            chat_focus_input = true;
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Invite popup

void PupOverlay::render_always(bool overlay_shown)
{
    // a new invite pops up once, it stays on the friend card after the popup is gone
    for (auto &e : ov.friends) {
        uint64 id = e.first.id();
        bool invited = (e.second.window_state & (window_state_lobby_invite | window_state_rich_invite)) != 0;
        if (invited && !invites_seen.count(id)) {
            invites_seen.insert(id);
            InvitePopup p{};
            p.friend_id = id;
            invite_popups.push_back(p);
        } else if (!invited && invites_seen.count(id)) {
            // accepted, declined or withdrawn
            invites_seen.erase(id);
            for (auto &p : invite_popups) {
                if (p.friend_id == id && p.closing < 0.0f) p.closing = 0.0f;
            }
        }
    }

    render_invite_popup(overlay_shown);
}

void PupOverlay::render_invite_popup(bool overlay_shown)
{
    ImGuiIO &io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    if (invite_popups.empty() || W <= 0 || H <= 0) return;

    constexpr float LIFETIME = 10.0f, IN_TIME = 0.35f, OUT_TIME = 0.3f;
    const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);

    InvitePopup &p = invite_popups.front();
    std::pair<const Friend, friend_window_state> *entry = nullptr;
    for (auto &e : ov.friends) {
        if ((uint64)e.first.id() == p.friend_id) entry = &e;
    }
    if (!entry && p.closing < 0.0f) p.closing = 0.0f; // the friend left

    p.age += dt;
    if (!overlay_shown) p.elapsed += dt; // don't run out while the player is answering
    if (p.closing < 0.0f && p.elapsed >= LIFETIME) p.closing = 0.0f;
    if (p.closing >= 0.0f) {
        p.closing += dt;
        if (p.closing >= OUT_TIME) {
            invite_popups.erase(invite_popups.begin());
            return;
        }
    }

    const float in_t = ease_out_cubic(p.age / IN_TIME);
    const float out_t = p.closing >= 0.0f ? 1.0f - ease_out_cubic(p.closing / OUT_TIME) : 1.0f;
    const float alpha = in_t * out_t;

    scale = ui_scale * std::clamp(H / 1080.0f, 0.8f, 2.0f);
    if (theme_name == CUSTOM_THEME_NAME) theme = custom_theme;
    ui::begin_frame(theme, scale);
    ImGui::PushFont(ov.font_default, ov.settings->overlay_appearance.font_size * scale);
    auto style_counts = push_imgui_style(theme, scale);

    const float card_w = S(460.0f), card_h = S(200.0f);
    // slides down into place and scales up a little
    const float pop = 0.92f + 0.08f * in_t;
    const ImVec2 size(card_w * pop, card_h * pop);
    const ImVec2 pos((W - size.x) * 0.5f, H * 0.32f - size.y * 0.5f - (1.0f - in_t) * S(30.0f));

    ImDrawList *fg = ImGui::GetForegroundDrawList();
    ui::draw_shadow(fg, pos, ImVec2(pos.x + size.x, pos.y + size.y), S(22.0f), S(26.0f), (theme.dark ? 0.6f : 0.3f) * alpha);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(22.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22.0f), S(18.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.panel);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.accent);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    // click-through while playing, the game has the mouse
    if (!overlay_shown) flags |= ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##pupberg_invite_popup", nullptr, flags)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        const std::string name = entry ? entry->first.name() : std::string("A friend");
        ImDrawList *dl = ImGui::GetWindowDrawList();

        // avatar with a pulsing ring
        ImVec2 c = ImGui::GetCursorScreenPos();
        const float r = S(30.0f);
        ImVec2 center(c.x + r, c.y + r);
        float pulse = 0.5f + 0.5f * std::sin(ui::ctx().time * 5.0f);
        dl->AddCircle(center, r + S(4.0f) + pulse * S(3.0f), to_u32(theme.accent, (0.35f + 0.4f * pulse) * alpha), 40, S(2.5f));
        ui::draw_avatar(dl, center, r, name, true);
        ImGui::Dummy(ImVec2(r * 2.0f, r * 2.0f));

        ImGui::SameLine(0.0f, S(16.0f));
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme.accent);
        ui::heading(name.c_str(), 1.3f);
        ImGui::PopStyleColor();
        ImGui::TextUnformatted("invited you to play!");
        ImGui::EndGroup();

        ui::spacer(10.0f);
        if (overlay_shown && entry) {
            const float bw = (ImGui::GetContentRegionAvail().x - S(12.0f)) * 0.5f;
            if (ui::button("Accept##invite_accept", ImVec2(bw, 0), ButtonKind::Primary, Icon::Paw)) {
                entry->second.window_state |= window_state_join;
                ov.has_friend_action.push(entry->first);
                if (p.closing < 0.0f) p.closing = 0.0f;
            }
            ImGui::SameLine(0.0f, S(12.0f));
            if (ui::button("Decline##invite_decline", ImVec2(bw, 0), ButtonKind::Ghost, Icon::Close)) {
                entry->second.window_state &= ~(window_state_lobby_invite | window_state_rich_invite);
                if (p.closing < 0.0f) p.closing = 0.0f;
            }
        } else {
            ui::text_muted("Press %s to answer", key_combo_text().c_str());
        }

        // time left
        ui::spacer(8.0f);
        float left = std::clamp(1.0f - p.elapsed / LIFETIME, 0.0f, 1.0f);
        char secs[16];
        snprintf(secs, sizeof(secs), "%ds", (int)std::ceil(LIFETIME - p.elapsed));
        ui::bone_progress(left, ImVec2(-1, S(10.0f)), overlay_shown ? nullptr : secs);
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    pop_imgui_style(style_counts);
    ImGui::PopFont();
}

void PupOverlay::render_side_windows()
{
    for (auto &entry : ov.friends) {
        ov.build_friend_window(entry.first, entry.second);
    }
    ov.render_gallery_window();
}

#endif // EMU_OVERLAY
