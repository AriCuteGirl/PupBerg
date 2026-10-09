#ifndef __INCLUDED_PUP_OVERLAY_H__
#define __INCLUDED_PUP_OVERLAY_H__

#ifdef EMU_OVERLAY

#include <memory>
#include <string>
#include <vector>
#include <set>
#include <mutex>
#include <chrono>

#include "pupberg/pup_theme.h"

class Steam_Overlay;

namespace pupberg { class ZeroTierClient; class InputFallback; }

// the PupBerg overlay frontend, drawn instead of Steam_Overlay::render_main_window()
// it reuses all the state of Steam_Overlay (friends, achievements, gallery, ...) through friendship
class PupOverlay
{
public:
    explicit PupOverlay(Steam_Overlay &overlay);
    ~PupOverlay();

    // called every rendered frame, before render()
    void tick(bool overlay_shown);
    // the full PupBerg UI, only call while the overlay is shown
    void render();
    // a small floating button over the classic overlay to come back to PupBerg
    void render_classic_switch();

private:
    enum class Page { Home, Friends, Achievements, Network, Gallery, Settings };

    Steam_Overlay &ov;

    Page page = Page::Home;
    std::chrono::steady_clock::time_point page_switch_time{};
    std::chrono::steady_clock::time_point open_time{};
    bool was_shown = false;

    std::string theme_name{};
    pupberg::Theme theme{};
    pupberg::Theme custom_theme{};
    float ui_scale = 1.0f;
    float scale = 1.0f; // ui_scale * resolution factor, valid during render()

    char friend_search[128]{};
    char ach_search[128]{};
    int ach_filter = 0;
    char zt_network_input[32]{};
    char peer_ip_input[64]{};
    bool zt_auto_join = false;
    bool zt_auto_join_done = false;
    std::vector<std::string> peer_ips{};

    std::unique_ptr<pupberg::ZeroTierClient> zt{};
    std::unique_ptr<pupberg::InputFallback> input{};
    std::mutex broadcasts_mutex{};
    std::set<uint32_t> added_broadcasts{};

    void load_prefs();
    void save_prefs();
    void select_theme(const std::string &name);
    void add_broadcast(uint32_t ip);
    void set_page(Page p);

    void render_sidebar(float width, float height);
    void render_banners();
    void render_home();
    void render_friends();
    void render_achievements();
    void render_network();
    void render_gallery();
    void render_settings();
    void render_side_windows();

    std::string key_combo_text() const;
};

#endif // EMU_OVERLAY

#endif // __INCLUDED_PUP_OVERLAY_H__
