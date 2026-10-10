#ifdef EMU_OVERLAY

#include "pupberg/pup_overlay.h"

#include "overlay/steam_overlay.h"
#include "dll/dll.h"

#include "pupberg/pup_ui.h"
#include "pupberg/pup_zerotier.h"
#include "pupberg/pup_input.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
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
    const ImVec2 win_pos((W - win_w) * 0.5f, (H - win_h) * 0.5f + (1.0f - open_t) * S(40.0f));
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
        static const char *titles[] = { "Home", "Friends", "Achievements", "Network", "Gallery", "Settings" };
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
        case Page::Achievements: render_achievements(); break;
        case Page::Network: render_network(); break;
        case Page::Gallery: render_gallery(); break;
        case Page::Settings: render_settings(); break;
        }
        ui::spacer(16.0f);
        ImGui::PopStyleVar();

        ImGui::EndChild();
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
        { "Achievements##nav", Icon::Trophy, Page::Achievements },
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
        ui::chip(zt_ip.empty() ? "ZeroTier off" : ("ZeroTier " + zt_ip).c_str(), zt_ip.empty() ? theme.text_muted : theme.success);
        ImGui::EndGroup();
    }
    ui::end_card();
    ui::spacer(14.0f);

    // stat tiles
    size_t unlocked = std::count_if(ov.achievements.begin(), ov.achievements.end(), [](const Overlay_Achievement &a) { return a.achieved; });
    const float tile_w = (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f;
    auto tile_header = [&](Icon icon, const char *label) {
        ImVec2 c = ImGui::GetCursorScreenPos();
        float sz = ImGui::GetFontSize() * 1.9f;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(c, ImVec2(c.x + sz, c.y + sz), to_u32(theme.accent, 0.16f), S(10.0f));
        ui::draw_icon(dl, icon, ImVec2(c.x + sz * 0.5f, c.y + sz * 0.5f), sz * 0.55f, to_u32(theme.accent));
        dl->AddText(ImVec2(c.x + sz + S(10.0f), c.y + (sz - ImGui::GetFontSize()) * 0.5f), to_u32(theme.text_muted), label);
        ImGui::Dummy(ImVec2(sz, sz));
    };

    ui::begin_card("##tile_ach", tile_w, true);
    tile_header(Icon::Trophy, "Achievements");
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%zu / %zu", unlocked, ov.achievements.size());
        ui::heading(buf, 1.4f);
        ui::bone_progress(ov.achievements.empty() ? 0.0f : (float)unlocked / (float)ov.achievements.size(), ImVec2(-1, S(10.0f)));
    }
    if (ui::end_card()) set_page(Page::Achievements);
    ImGui::SameLine(0.0f, gap);

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
    tile_header(Icon::Network, "ZeroTier");
    ui::heading(zt_ip.empty() ? "Offline" : zt_ip.c_str(), 1.4f);
    ui::text_muted(!zt_state.token_found ? "Needs the auth token" :
                   !zt_state.service_online ? "Service not running" : (zt_ip.empty() ? "Join a network" : "Connected"));
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
    if (ov.settings->overlay_show_button_test_achievement) {
        if (ui::button("Test achievement##qa_test", ImVec2(0, 0), ButtonKind::Soft, Icon::Trophy)) ov.show_test_achievement();
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
                       "Join the same ZeroTier network in the Network tab and they'll pop up in a few seconds.");
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
            state.window_state |= window_state_show;
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
// Achievements

void PupOverlay::render_achievements()
{
    auto &achs = ov.achievements;
    size_t unlocked = std::count_if(achs.begin(), achs.end(), [](const Overlay_Achievement &a) { return a.achieved; });

    if (achs.empty()) {
        ui::begin_card("##no_ach");
        ui::heading("No achievements here", 1.2f);
        ui::text_muted("This game has no achievements, or steam_settings/achievements.json is missing.");
        ui::end_card();
        return;
    }

    ui::begin_card("##ach_summary");
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "%zu of %zu unlocked", unlocked, achs.size());
        ImGui::TextUnformatted(buf);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(6.0f));
        ImGui::TextColored(theme.accent, "%d%%", (int)std::round(100.0f * unlocked / achs.size()));
        ui::bone_progress((float)unlocked / achs.size(), ImVec2(-1, S(14.0f)));
    }
    ui::end_card();
    ui::spacer(12.0f);

    static const char *filters[] = { "All", "Unlocked", "Locked" };
    ui::segmented("##ach_filter", filters, 3, &ach_filter);
    ImGui::SameLine(0.0f, S(14.0f));
    ImGui::PushItemWidth(std::min(S(320.0f), ImGui::GetContentRegionAvail().x));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(3.0f));
    ImGui::InputTextWithHint("##ach_search", "Search achievements...", ach_search, sizeof(ach_search));
    ImGui::PopItemWidth();
    ui::spacer(10.0f);

    // unlocked first (newest first), then locked by rarity
    std::vector<size_t> order{};
    for (size_t i = 0; i < achs.size(); ++i) {
        const auto &a = achs[i];
        if (ach_filter == 1 && !a.achieved) continue;
        if (ach_filter == 2 && a.achieved) continue;
        bool hidden = a.hidden && !a.achieved;
        if (!contains_insensitive(a.title, ach_search) && (hidden || !contains_insensitive(a.description, ach_search))) continue;
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t l, size_t r) {
        const auto &a = achs[l], &b = achs[r];
        if (a.achieved != b.achieved) return a.achieved;
        if (a.achieved) return a.unlock_time > b.unlock_time;
        return a.unlock_percentage > b.unlock_percentage;
    });

    const float gap = S(12.0f);
    const float avail = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)((avail + gap) / (S(330.0f) + gap)));
    const float card_w = (avail - gap * (cols - 1)) / cols;
    const float icon_sz = S(56.0f);

    int col = 0;
    for (size_t i : order) {
        auto &x = achs[i];
        const bool achieved = x.achieved;
        const bool hidden = x.hidden && !achieved;

        if (x.unlock_percentage < 0.0f && x.name.size()) {
            x.unlock_percentage = static_cast<float>(get_steam_client()->steam_user_stats->GetAchievementUnlockPercentage(x.name.c_str()));
        }
        ov.try_load_ach_icon(x, achieved, ov.settings->paginated_achievements_icons == 0);

        if (col > 0) ImGui::SameLine(0.0f, gap);
        ImGui::PushID((int)i);
        ui::begin_card("##ach_card", card_w);

        ImVec2 ic = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(icon_sz, icon_sz));
        ImDrawList *dl = ImGui::GetWindowDrawList();
        auto *icon_rsrc = achieved ? x.icon : x.icon_gray;
        uint64_t tex = icon_rsrc ? icon_rsrc->GetResourceId() : 0;
        if (tex) {
            dl->AddImageRounded((ImTextureID)tex, ic, ImVec2(ic.x + icon_sz, ic.y + icon_sz), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, S(12.0f));
        } else {
            dl->AddRectFilled(ic, ImVec2(ic.x + icon_sz, ic.y + icon_sz), to_u32(achieved ? theme.accent : theme.border, achieved ? 0.25f : 0.6f), S(12.0f));
            ui::draw_icon(dl, achieved ? Icon::Trophy : Icon::Lock, ImVec2(ic.x + icon_sz * 0.5f, ic.y + icon_sz * 0.5f), icon_sz * 0.5f,
                          to_u32(achieved ? theme.accent : theme.text_muted));
        }
        if (achieved) {
            // rare achievements get a golden ring
            bool rare = x.unlock_percentage >= 0.0f && x.unlock_percentage <= 10.0f;
            dl->AddRect(ImVec2(ic.x - 1, ic.y - 1), ImVec2(ic.x + icon_sz + 1, ic.y + icon_sz + 1),
                        rare ? IM_COL32(255, 205, 80, 255) : to_u32(theme.accent, 0.7f), S(12.0f), 0, rare ? S(3.0f) : S(1.5f));
        }

        ImGui::SameLine(0.0f, S(12.0f));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + (ui::card_inner_right() - ImGui::GetCursorScreenPos().x));
        ImGui::TextUnformatted(hidden ? "Hidden achievement" : x.title.c_str());
        ui::text_muted("%s", hidden ? "Keep playing to sniff this one out." : x.description.c_str());
        if (!achieved && x.max_progress > 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%u / %u", x.progress, x.max_progress);
            ui::bone_progress((float)x.progress / (float)x.max_progress, ImVec2(-1, S(9.0f)), buf);
        }
        if (achieved) {
            char date[80]{};
            time_t t = (time_t)x.unlock_time;
            struct tm tm_buf{};
#ifdef _MSC_VER
            localtime_s(&tm_buf, &t);
#else
            localtime_r(&t, &tm_buf);
#endif
            if (!std::strftime(date, sizeof(date), ov.settings->overlay_appearance.ach_unlock_datetime_format.c_str(), &tm_buf)) {
                std::strftime(date, sizeof(date), "%Y/%m/%d", &tm_buf);
            }
            ui::chip(date, theme.success);
        } else {
            ui::chip("Locked", theme.text_muted);
        }
        if (x.unlock_percentage >= 0.0f) {
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(3.0f));
            ImGui::TextColored(theme.text_muted, "%.1f%% of players", std::max(0.1f, x.unlock_percentage));
        }
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        ui::end_card();
        ImGui::PopID();

        if (++col >= cols) {
            col = 0;
            ui::spacer(gap / ui::ctx().scale - 8.0f);
        }
    }
    if (order.empty()) ui::text_muted("Nothing matches this filter.");
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
        ui::spacer(4.0f);
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
void PupOverlay::render_side_windows()
{
    for (auto &entry : ov.friends) {
        ov.build_friend_window(entry.first, entry.second);
    }
    ov.render_gallery_window();
}

#endif // EMU_OVERLAY
